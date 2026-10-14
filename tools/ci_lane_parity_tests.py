"""Pins what a pull request runs against what a push to main runs.

Pull requests from the trusted operator run some lanes on self-hosted runners for speed, while
main pushes always run GitHub-hosted lanes. A pull request that is green on its lanes should not
turn main red, so every check a main push runs must also run on each kind of pull request, with
the same test selection, tag filters, test environment and Bazel flags. This test evaluates each
workflow's job and step conditions for a main push, a trusted pull request and an untrusted pull
request, then compares the Bazel invocations that can fail the run.

Every difference must be listed in an allowlist below with the reason it is acceptable. An entry
that no longer matches a difference also fails, so the lists shrink as gaps close.
"""

import hashlib
import json
from pathlib import Path
import re
import shlex
import unittest

from python.runfiles import runfiles


# The shared target selection: `//...` on a fallback, otherwise the affected labels. Steps that use
# it are compared by this symbol, since the labels are only known when the workflow runs.
SELECTED_TARGETS = "@selected_targets"

# Workflows that run on a push to main without running on pull requests at all, or with a
# narrower pull request trigger. Keyed by (workflow, difference).
TRIGGER_ALLOWLIST = {
    ("badges.yaml", "push-only"):
        "Publishes line-count badges for main; it runs no build or test.",
    ("codeql.yml", "push-only"):
        "Static analysis of main; its findings go to code scanning and do not gate a merge.",
    ("deploy_docs.yaml", "push-only"):
        "Builds the documentation site from main for a later manual deploy. Doxygen warnings "
        "are not errors, so a pull request cannot make it fail through source changes alone.",
    ("bcr_preflight.yml", "pull-request-paths"):
        "Pull requests run the BCR preflight only when they touch its listed inputs, to save "
        "hosted macOS capacity; main runs it on every push.",
}

# Jobs that run on a main push but not on one kind of pull request, mapped to the job that runs
# in their place there. The replacement's Bazel invocations are compared step by step.
JOB_REPLACEMENTS = {
    ("main.yml", "macos", "trusted-pr"): "macos-self-hosted",
}

# Hosted jobs that run some of a replaced job's steps on the replaced job's own runner image, for
# the pull requests that affect those steps' targets. Keyed like JOB_REPLACEMENTS; the value is
# (subset job, steps it covers). A covered step must run with exactly main's Bazel invocation, so
# it is compared with the subset job instead of the replacement. The subset runs only when the
# PR's affected set reaches the covered tests (or falls back to //...); a PR that cannot change
# them leaves main's result for them unchanged, apart from flakes main's //... run can still hit.
HOSTED_SUBSETS = {
    ("main.yml", "macos", "trusted-pr"): ("macos-browser", ("Test (browser targets)",)),
}

# Differences between a main step and the hosted subset's copy of it. Keyed by (workflow, main
# job, subset job, step, kind, value). Only build-scope differences belong here: the subset must
# test exactly what main tests.
SUBSET_ALLOWLIST = {
    ("main.yml", "macos", "macos-browser", "Test (browser targets)", "flag-pr-only",
     "--build_tests_only"):
        "Build scope only: main's job builds the whole selection in its Build step first, and "
        "the subset has no Build step, so it builds only what the selected tests need. The "
        "tests themselves, their filters and environment are main's.",
}

# Setup step inputs whose value in a hosted subset is fixed instead of copied from main's job. The
# subset runs only for pull requests, which must not save the Actions cache, so it states that
# directly rather than repeating main's main-only expression.
SUBSET_SETUP_REQUIRED_INPUTS = {("Setup Bazel", "cache-save"): "false"}

# Steps a hosted subset runs that main's job does not, with the reason each cannot change what
# the covered steps test.
SUBSET_EXTRA_STEPS = {}

# Jobs that run on a main push and on no kind of pull request, without a replacement.
MAIN_ONLY_JOBS = {}

# Differences between a main job and its replacement. Keyed by (workflow, main job, replacement,
# step, kind, value); the reason says why the difference cannot make main red after a green PR,
# or names the accepted risk.
INVOCATION_ALLOWLIST = {
    # The hosted image has no offline Metal compiler, so its lane drops these targets. The
    # self-hosted runner has one and runs them, so the pull request runs more than main.
    ("main.yml", "macos", "macos-self-hosted", "Test", "tag-filter-main-only",
     "-requires_metal_toolchain"):
        "Main runs fewer tests: the hosted image cannot compile offline Metal shaders.",
    ("main.yml", "macos", "macos-self-hosted", "Test (browser targets)", "tag-filter-main-only",
     "-requires_metal_toolchain"):
        "Main runs fewer tests: the hosted image cannot compile offline Metal shaders.",
    # Browser cases quarantined on the hosted runner alone (#1691, #1702). Main skips them; the
    # pull request runs them, so they cannot turn main red. Remove with the quarantine.
    ("main.yml", "macos", "macos-self-hosted", "Test", "test-env-main-only",
     "DONNER_HOSTED_MACOS_QUARANTINE=1"):
        "Main skips the quarantined cases that the pull request runs (#1691, #1702).",
    ("main.yml", "macos", "macos-self-hosted", "Test (browser targets)", "test-env-main-only",
     "DONNER_HOSTED_MACOS_QUARANTINE=1"):
        "Main skips the quarantined cases that the pull request runs (#1691, #1702).",
    # Records host load and process call graphs only when a browser test reaches its deadline.
    # A persistent self-hosted runner never publishes them.
    ("main.yml", "macos", "macos-self-hosted", "Test", "test-env-main-only",
     "DONNER_BROWSER_STALL_DIAGNOSTICS=1"):
        "Diagnostics only: it changes what a timed-out test records, not whether it passes.",
    ("main.yml", "macos", "macos-self-hosted", "Test (browser targets)", "test-env-main-only",
     "DONNER_BROWSER_STALL_DIAGNOSTICS=1"):
        "Diagnostics only: it changes what a timed-out test records, not whether it passes.",
    # The self-hosted runner ignores its host's user bazelrc and uses a remote cache that it
    # must not silently fall back from.
    ("main.yml", "macos", "macos-self-hosted", "*", "startup-pr-only", "--nohome_rc"):
        "Runner-specific: keeps the host's interactive bazelrc out of CI.",
    ("main.yml", "macos", "macos-self-hosted", "*", "flag-pr-only",
     "--remote_upload_local_results=true"):
        "Runner-specific remote cache setting; it does not change what runs.",
    ("main.yml", "macos", "macos-self-hosted", "*", "flag-pr-only",
     "--remote_local_fallback=false"):
        "Runner-specific remote cache setting; it does not change what runs.",
    ("main.yml", "macos", "macos-self-hosted", "*", "flag-pr-only",
     "--incompatible_remote_local_fallback_for_remote_cache=false"):
        "Runner-specific remote cache setting; it does not change what runs.",
    ("main.yml", "macos", "macos-self-hosted", "*", "flag-pr-only", "--remote_timeout=5s"):
        "Runner-specific remote cache setting; it does not change what runs.",
    # The value is built at run time from the masked secret, so only its empty literal prefix
    # is compared.
    ("main.yml", "macos", "macos-self-hosted", "*", "runtime-env-pr-only",
     "BAZEL_MACOS_BUILD_FLAGS="):
        "Adds the masked remote cache address to the self-hosted build flags.",
    # The hosted runner has a paravirtual GPU whose Metal validation layer cannot check texture
    # usage, so the hosted lane picks its validation profile at run time. The self-hosted runner
    # requires full validation instead, which is stricter.
    ("main.yml", "macos", "macos-self-hosted", "*", "runtime-env-main-only",
     "BAZEL_MACOS_TEST_FLAGS=--test_env=HOME --test_env=MTL_SHADER_VALIDATION_TEXTURE_USAGE=0"):
        "Main may relax one Metal texture check on a paravirtual GPU; the pull request runs "
        "full validation.",
    # Setup the hosted runner needs and the self-hosted runner has already: the host provides
    # Bazelisk, Homebrew LLVM and the Xcode tools ("Prepare self-hosted macOS toolchain"),
    # keeps Bazel's repository and action state between runs, and uses its own remote cache
    # ("Configure masked remote cache"). The Bazel steps then fetch what they need.
    **{("main.yml", "macos", "macos-self-hosted", step, "step-main-only", ""):
       "Hosted runner setup only; the self-hosted runner is provisioned with it, and it "
       "changes no test, filter or flag."
       for step in ("Install clang-tidy", "Setup Bazel", "Restore Bazel action cache",
                    "Configure BuildBuddy remote cache", "Fetch Metal validation dependencies")},
    ("main.yml", "macos", "macos-self-hosted", "Select Metal validation profile", "step-main-only",
     ""):
        "Main picks its Metal validation profile; the pull request instead requires full "
        "validation in 'Require full Metal shader validation'.",
}

# Differences in how a job that runs on both main and a pull request runs Bazel in each, keyed
# by (workflow, job, scenario, step, kind, value).
SHARED_JOB_ALLOWLIST = {}

# (action, input) pairs whose value differs between main and pull requests by policy, pinned by
# the security policy tests: Actions cache saves are main-only, and BuildBuddy write access
# follows pull request trust. See _other_step_view.
_POLICY_EVENT_INPUTS = frozenset({
    ("bazel-contrib/setup-bazel", "cache-save"),
    ("./.github/actions/buildbuddy-cache", "trusted-pr"),
})

_EVENT_ROUTING_REASON = (
    "Event routing: the step decides from the event which targets, scope or runs apply. Its "
    "outputs are the scenario inputs this test models, so its own script differs by design.")

# The kinds of pull request each check is evaluated for (see SCENARIOS).
PULL_REQUEST_SCENARIOS = ("trusted-pr", "untrusted-pr", "renovate-pr")

# Non-Bazel steps of a shared job whose rendered definition differs between main and some kind
# of pull request: (workflow, job, step) to {scenario: digest of the rendered main and pull
# request definitions}. Any edit to the step, or a difference in another scenario, changes the
# observed digests, so the entry must be reviewed and updated.
_SHARED_STEP_DIGESTS = {
    ("bcr_preflight.yml", "prepare",
     "Select the preflight scope"): dict.fromkeys(PULL_REQUEST_SCENARIOS, "642c5d1b39d0fcb8"),
    ("cmake.yml", "gatekeeper",
     "Detect docs-only PR"): dict.fromkeys(PULL_REQUEST_SCENARIOS, "9ecf5fb96869b275"),
    ("cmake.yml", "gatekeeper",
     "Detect duplicate push (PR already open for this branch)"): dict.fromkeys(PULL_REQUEST_SCENARIOS, "a72c9791c92a41a9"),
    ("lint.yml", "gatekeeper",
     "Detect duplicate push (PR already open for this branch)"): dict.fromkeys(PULL_REQUEST_SCENARIOS, "a72c9791c92a41a9"),
    ("main.yml", "determine-targets",
     "Determine affected Bazel targets"): dict.fromkeys(PULL_REQUEST_SCENARIOS, "b2774a4e49fa714f"),
    ("main.yml", "determine-targets",
     "Extract SHA for hash caching"): dict.fromkeys(PULL_REQUEST_SCENARIOS, "75cf06ec1a7a5082"),
    ("main.yml", "gatekeeper",
     "Detect docs-only PR"): dict.fromkeys(PULL_REQUEST_SCENARIOS, "9ecf5fb96869b275"),
    ("main.yml", "gatekeeper",
     "Detect duplicate push (PR already open for this branch)"): dict.fromkeys(PULL_REQUEST_SCENARIOS, "694bfdca0dd35e4c"),
}

SHARED_STEP_ALLOWLIST = {
    (workflow, job, step, scenario, digest): _EVENT_ROUTING_REASON
    for (workflow, job, step), digests in _SHARED_STEP_DIGESTS.items()
    for scenario, digest in digests.items()
}

# Steps of one job that run on a main push but not on one kind of pull request.
_TRUSTED_LINUX_REASON = (
    "operator decision 2026-10-13: trusted PRs run the Linux Build and Test steps only on the "
    "self-hosted lane, for speed; hosted Linux covers main pushes and untrusted PRs")

_CACHE_WRITER_REASON = (
    "Remote cache wiring only: main and trusted PRs read and write the BuildBuddy cache, Renovate "
    "PRs read it through the read-only step, and other PRs build without it. A cache changes how "
    "fast results arrive, not which tests run or what they assert, and a colder PR build is "
    "stricter than main's.")

STEP_ALLOWLIST = {
    **{(workflow, job, "Configure BuildBuddy remote cache", scenario): _CACHE_WRITER_REASON
       for workflow, job in (("editor_wasm.yml", "build"), ("main.yml", "linux"),
                             ("main.yml", "macos"))
       for scenario in ("untrusted-pr", "renovate-pr")},
    # Accepted risk: the self-hosted Linux lane builds with another architecture and the
    # hermetic LLVM toolchain with libc++, so a failure specific to the hosted runner's
    # toolchain or speed first shows on main.
    ("main.yml", "linux", "Build", "trusted-pr"): _TRUSTED_LINUX_REASON,
    ("main.yml", "linux", "Test", "trusted-pr"): _TRUSTED_LINUX_REASON,
}

# Steps main runs that a kind of pull request skips, but that a different job runs on that pull
# request instead. Keyed like JOB_REPLACEMENTS; the value is (counterpart job, steps). Each step's
# Bazel invocation is compared with the counterpart's step of the same name, so a narrower target
# selection, tag filter or test environment on the pull request side fails even though the main
# step itself is in STEP_ALLOWLIST.
COUNTERPART_STEPS = {
    ("main.yml", "linux", "trusted-pr"): ("linux-self-hosted", ("Build", "Test")),
}

# Differences between a main step and its counterpart. Keyed by (workflow, main job, counterpart,
# step, kind, value).
def _counterpart_entries(main_job, counterpart, entries, reason):
    return {("main.yml", main_job, counterpart, step, kind, value): reason
            for step, kind, value in entries}


COUNTERPART_ALLOWLIST = {
    **_counterpart_entries("linux", "linux-self-hosted", [
        ("*", "flag-pr-only", "--config=latest_llvm"),
    ], "Accepted toolchain difference: the self-hosted ARM64 lane builds with the hermetic LLVM "
       "toolchain and libc++, while hosted Linux uses the runner image's clang and libstdc++."),
    **_counterpart_entries("linux", "linux-self-hosted", [
        ("*", "flag-pr-only", "--strategy=TestRunner=remote"),
        ("*", "flag-pr-only", "--remote_local_fallback=false"),
        ("*", "flag-pr-only", "--incompatible_remote_local_fallback_for_remote_cache=false"),
        ("*", "flag-pr-only", "--remote_timeout=60s"),
        ("*", "flag-pr-only", "--remote_download_outputs=minimal"),
        ("Test", "flag-pr-only",
         r"--remote_download_regex=.*(test\\.log|test\\.xml|outputs\\.zip)$"),
    ], "Runner-specific remote execution settings; they choose where actions run and which "
       "outputs come back, not what is built or tested."),
    **_counterpart_entries("linux", "linux-self-hosted", [
        ("*", "flag-pr-only", "--nobuild_event_json_file_path_conversion"),
        ("Build", "flag-pr-only", "--build_event_json_file=$DIAG_DIR/build/bep.json"),
        ("Build", "flag-pr-only", "--execution_log_compact_file=$DIAG_DIR/build/exec.log.zst"),
        ("Build", "flag-pr-only", "--profile=$DIAG_DIR/build/profile.gz"),
        ("Test", "flag-pr-only", "--build_event_json_file=$DIAG_DIR/test/bep.json"),
        ("Test", "flag-pr-only", "--execution_log_compact_file=$DIAG_DIR/test/exec.log.zst"),
        ("Test", "flag-pr-only", "--profile=$DIAG_DIR/test/profile.gz"),
    ], "Diagnostics only: the self-hosted lane records profiles and logs for slow-run triage."),
    **_counterpart_entries("linux", "linux-self-hosted", [
        ("Test", "tag-filter-main-only", "-ci-remote-gpu"),
        ("Test", "tag-filter-pr-only", "-local-gpu-isolated"),
        ("Test", "target-main-only", SELECTED_TARGETS),
        ("Test", "target-pr-only",
         "$(python3 tools/ci_remote_gpu_targets.py %s)" % SELECTED_TARGETS),
    ], "The self-hosted lane swaps each locally isolated GPU suite for its remote wrapper "
       "(tools/ci_remote_gpu_targets.py), which runs the same implementation and shards, so it "
       "keeps the remote wrappers and drops the local-only targets; hosted Linux does the "
       "reverse. The local-display targets this drops run in the hosted job's Xvfb step."),
}

# Each scenario sets every job output a condition reads; an output not modelled here raises.
# `trusted_pr` and `renovate_pr` are the .github/actions/pr-trust outputs, which both the
# self-hosted routing and the remote cache wiring read; a push gets "false" for both. A PR here
# touches no release inputs, so the BCR preflight runs its slim scope (`full_matrix`), it does
# not reach the WebGPU reference corpus (`wgpu_reference`), and it does reach a browser test
# (`browser_affected`).
_PULL_REQUEST = {
    "github.event_name": "pull_request",
    "github.ref": "refs/pull/1/merge",
    "fallback": "false",
    "affected": "//donner/editor/wasm/tests:chromium_remote_smoke",
    "browser_affected": "true",
    "wgpu_reference": "false",
    "full_matrix": "false",
}

# Steps that run a Bazel build or test and also contain a line that can turn a failure into
# success (see failure_masks). Keyed by (workflow, job, step, line).
FAILURE_SWALLOW_ALLOWLIST = {}

SCENARIOS = {
    "main-push": {
        "github.event_name": "push",
        "github.ref": "refs/heads/main",
        "use_self_hosted": "false",
        "trusted_pr": "false",
        "renovate_pr": "false",
        "fallback": "true",
        "affected": "",
        "wgpu_reference": "",
        "full_matrix": "false",
        "browser_affected": "",
    },
    "trusted-pr": dict(_PULL_REQUEST, use_self_hosted="true", trusted_pr="true",
                       renovate_pr="false"),
    # A trusted PR whose affected set reaches no browser test.
    "trusted-pr-native-only": dict(_PULL_REQUEST, use_self_hosted="true", trusted_pr="true",
                                   renovate_pr="false", affected="//donner/base:base_tests",
                                   browser_affected="false"),
    # A trusted PR that falls back to //... (an infrastructure change).
    "trusted-pr-fallback": dict(_PULL_REQUEST, use_self_hosted="true", trusted_pr="true",
                                renovate_pr="false", fallback="true", affected="",
                                browser_affected=""),
    "untrusted-pr": dict(_PULL_REQUEST, use_self_hosted="false", trusted_pr="false",
                         renovate_pr="false"),
    "renovate-pr": dict(_PULL_REQUEST, use_self_hosted="false", trusted_pr="false",
                        renovate_pr="true"),
}


# Step outputs a condition may read, with the value used in every scenario and why. A step
# output not listed here fails the test, so a new one is reviewed before it can gate a check.
_CACHE_HIT = ("", "A cache restore result only gates that cache's save step; a miss runs every "
                  "such step.")

STEP_OUTPUTS = {
    **{"steps.%s.outputs.cache-hit" % step_id: _CACHE_HIT
       for step_id in ("base-hashes", "bazel-cache", "ccache", "cmake-deps", "npm-cache")},
    "steps.browser_gpu_failure.outputs.run_probe": (
        "", "Gates the advisory browser GPU comparisons, which run only after a failure and "
            "cannot fail the job."),
    "steps.pr-meta.outputs.base_sha": (
        "set", "Main pushes and pull requests both have a base revision for bazel-diff."),
}

# Job outputs a condition may read, mapped to the scenario key that holds each one's value.
_MODELLED_OUTPUTS = {
    "needs.gatekeeper.outputs.trusted_pr": "trusted_pr",
    "needs.gatekeeper.outputs.renovate_pr": "renovate_pr",
    "needs.trust.outputs.trusted_pr": "trusted_pr",
    "needs.trust.outputs.renovate_pr": "renovate_pr",
    "needs.determine-targets.outputs.fallback": "fallback",
    "needs.determine-targets.outputs.affected": "affected",
    "needs.determine-targets.outputs.wgpu_reference": "wgpu_reference",
    "needs.determine-targets.outputs.browser_affected": "browser_affected",
    "needs.prepare.outputs.full_matrix": "full_matrix",
}

BAZEL_COMMANDS = ("build", "test")


def _read(path):
    resolved = runfiles.Create().Rlocation("donner/%s" % path)
    with open(resolved, encoding="utf-8") as handle:
        return handle.read()


def _workflow_paths():
    resolved = runfiles.Create().Rlocation("donner/.github/workflows/main.yml")
    directory = Path(resolved).parent
    return sorted(directory.glob("*.yml")) + sorted(directory.glob("*.yaml"))


# --- A small YAML reader for the subset GitHub workflows here use. -----------------------------


# One token of a YAML or shell line: an escaped character, a Bash ANSI-C quoted string
# (`$'...'`, where a backslash escapes the next character), a single- or double-quoted string,
# or any other character. A backslash escapes the next character except inside plain single
# quotes, as in Bash and YAML double-quoted strings.
_COMMENT_SCAN = re.compile(r"""\\.|\$'(?:\\.|[^'\\])*'?|'[^']*'?|"(?:\\.|[^"\\])*"?|.""",
                           re.DOTALL)


def _strip_comment(value, yaml=False):
    """A YAML or shell line without its trailing comment, outside quotes and escapes.

    A comment starts at a `#` token at the start of the line or after a space or tab. In shell
    text the blank must be unescaped, so `label\\ #1` keeps the `#` in the word. A YAML plain
    scalar has no escapes, so with `yaml` the `#` after an escaped blank still starts a comment.
    """
    after_blank = True
    for match in _COMMENT_SCAN.finditer(value):
        token = match.group()
        if token == "#" and after_blank:
            return value[:match.start()].rstrip()
        after_blank = (token[-1] if yaml else token) in (" ", "\t")
    return value


# A backslash-newline continues a shell line only when the run of backslashes before the newline
# has odd length; an even run ends in an escaped backslash.
_CONTINUATION = re.compile(r"(?<!\\)((?:\\\\)*)\\\n\s*")


def _join_continuations(script):
    """A shell script with each backslash-newline continuation joined into one line."""
    return _CONTINUATION.sub(r"\1 ", script)


def _scalar(value):
    value = _strip_comment(value.strip(), yaml=True)
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
        return value[1:-1]
    if value.startswith("[") and value.endswith("]"):
        return [_scalar(part) for part in value[1:-1].split(",") if part.strip()]
    return value


class _Lines:
    def __init__(self, text):
        self.lines = text.splitlines()
        self.index = 0

    def skip_blank(self):
        while self.index < len(self.lines):
            stripped = self.lines[self.index].strip()
            if stripped and not stripped.startswith("#"):
                return
            self.index += 1

    def peek_indent(self):
        self.skip_blank()
        if self.index >= len(self.lines):
            return -1
        line = self.lines[self.index]
        return len(line) - len(line.lstrip(" "))


def _block_scalar(lines, parent_indent, style):
    collected = []
    block_indent = None
    while lines.index < len(lines.lines):
        line = lines.lines[lines.index]
        if line.strip():
            indent = len(line) - len(line.lstrip(" "))
            if indent <= parent_indent:
                break
            if block_indent is None:
                block_indent = indent
            collected.append(line[block_indent:])
        else:
            collected.append("")
        lines.index += 1
    while collected and not collected[-1]:
        collected.pop()
    if style.startswith(">"):
        return " ".join(part.strip() for part in collected if part.strip())
    return "\n".join(collected) + "\n"


def _value(lines, raw, indent):
    raw = raw.strip()
    if raw in ("|", "|-", ">", ">-"):
        return _block_scalar(lines, indent, raw)
    if raw == "" or raw.startswith("#"):
        child = lines.peek_indent()
        if child > indent or (child == indent and lines.index < len(lines.lines)
                              and lines.lines[lines.index].lstrip().startswith("- ")):
            return _node(lines, child)
        return None
    return _scalar(raw)


def _sequence_item(lines, indent):
    content = lines.lines[lines.index].strip()[1:].strip()
    lines.index += 1
    if not re.match(r"^[A-Za-z0-9_.-]+:(\s|$)", content):
        return _scalar(content)
    key, _, rest = content.partition(":")
    mapping = {key: _value(lines, rest, indent + 2)}
    if lines.peek_indent() == indent + 2:
        mapping.update(_node(lines, indent + 2))
    return mapping


def _at_sequence_item(lines, indent):
    return (lines.peek_indent() == indent
            and lines.lines[lines.index].strip().startswith("-"))


def _node(lines, indent):
    lines.skip_blank()
    if _at_sequence_item(lines, indent):
        items = []
        while _at_sequence_item(lines, indent):
            items.append(_sequence_item(lines, indent))
        return items
    mapping = {}
    while lines.peek_indent() == indent:
        content = lines.lines[lines.index].strip()
        if content.startswith("- "):
            break
        match = re.match(r"^([^:]+):(\s.*|)$", content)
        if not match:
            raise ValueError("unsupported YAML line: %r" % content)
        lines.index += 1
        mapping[_scalar(match.group(1))] = _value(lines, match.group(2), indent)
    return mapping


def parse_workflow(text):
    """Parse a workflow, raising on any line the reader does not understand.

    The reader stops a mapping or sequence at the first line it cannot place, so a construct it
    does not support (a multi-line plain scalar, a flow mapping over several lines) would end the
    parse early and silently drop the rest of the file. Require every line to be consumed.
    """
    lines = _Lines(text)
    workflow = _node(lines, 0)
    lines.skip_blank()
    if lines.index < len(lines.lines):
        raise ValueError("unsupported YAML at line %d: %r"
                         % (lines.index + 1, lines.lines[lines.index]))
    return workflow


# --- Expression evaluation for `if:` conditions. -------------------------------------------------


_TOKEN = re.compile(r"\s*(?:(?P<string>'(?:[^']|'')*')|(?P<op>==|!=|&&|\|\||[!(),])|"
                    r"(?P<word>[A-Za-z_][A-Za-z0-9_.*-]*))")


class _Expression:
    def __init__(self, text, context):
        text = text.strip()
        if text.startswith("${{") and text.endswith("}}"):
            text = text[3:-2]
        self.tokens = []
        position = 0
        while position < len(text):
            if not text[position:].strip():
                break
            match = _TOKEN.match(text, position)
            if not match or match.end() == position:
                raise ValueError("cannot evaluate expression %r" % text)
            self.tokens.append(match)
            position = match.end()
        self.position = 0
        self.context = context

    def _peek(self):
        if self.position < len(self.tokens):
            token = self.tokens[self.position]
            return token.group("op") or token.group("word") or token.group("string")
        return None

    def _take(self):
        token = self.tokens[self.position]
        self.position += 1
        return token

    def evaluate(self):
        value = self._or()
        if self.position != len(self.tokens):
            raise ValueError("trailing tokens in expression")
        return value

    def _or(self):
        value = self._and()
        while self._peek() == "||":
            self._take()
            right = self._and()
            value = value if _truthy(value) else right
        return value

    def _and(self):
        value = self._compare()
        while self._peek() == "&&":
            self._take()
            right = self._compare()
            value = right if _truthy(value) else value
        return value

    def _compare(self):
        value = self._unary()
        while self._peek() in ("==", "!="):
            operator = self._take().group("op")
            right = self._unary()
            equal = str(value).lower() == str(right).lower()
            value = equal if operator == "==" else not equal
        return value

    def _unary(self):
        if self._peek() == "!":
            self._take()
            return not _truthy(self._unary())
        return self._primary()

    def _primary(self):
        token = self._take()
        if token.group("op") == "(":
            value = self._or()
            self._take()
            return value
        if token.group("string") is not None:
            return token.group("string")[1:-1].replace("''", "'")
        word = token.group("word")
        if self._peek() == "(":
            self._take()
            arguments = []
            while self._peek() != ")":
                arguments.append(self._or())
                if self._peek() == ",":
                    self._take()
            self._take()
            return self._call(word, arguments)
        if word in ("true", "false"):
            return word == "true"
        return self.context(word)

    @staticmethod
    def _call(name, arguments):
        # Conditions are evaluated for a run in which every earlier step succeeded.
        if name in ("always", "success"):
            return True
        if name in ("cancelled", "failure"):
            return False
        if name == "contains":
            haystack, needle = arguments
            if isinstance(haystack, list):
                return needle in haystack
            return str(needle).lower() in str(haystack).lower()
        raise ValueError("unsupported function %s" % name)


def _truthy(value):
    return value not in (False, "", None, 0)


def _context(scenario):
    values = SCENARIOS[scenario]

    def lookup(name):
        if name in ("github.event_name", "github.ref"):
            return values[name]
        if name.startswith("needs.gatekeeper.outputs.use_self_hosted_"):
            return values["use_self_hosted"]
        if name in ("needs.gatekeeper.outputs.is_duplicate_push",
                    "needs.gatekeeper.outputs.docs_only"):
            return "false"
        if name in _MODELLED_OUTPUTS:
            return values[_MODELLED_OUTPUTS[name]]
        if name.startswith("steps.") and name.endswith(".outcome"):
            return "success"
        if name.endswith(".labels.*.name"):
            return []
        if name in STEP_OUTPUTS:
            return STEP_OUTPUTS[name][0]
        # An output that is not modelled above can decide which checks run, so it fails the
        # test until it is reviewed and listed.
        raise ValueError("unmodelled context %s" % name)

    return lookup


def runs(condition, scenario):
    if condition is None:
        return True
    if isinstance(condition, bool):
        return condition
    return _truthy(_Expression(str(condition), _context(scenario)).evaluate())


# --- Bazel invocation extraction. ----------------------------------------------------------------


def _expand(text, environment):
    def replace(match):
        name = match.group(1) or match.group(2)
        return environment.get(name, match.group(0))

    return re.sub(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}|\$([A-Za-z_][A-Za-z0-9_]*)", replace, text)


def _split_words(text):
    try:
        return shlex.split(text, comments=True)
    except ValueError:
        return text.split()


_SELECTION = re.compile(
    r'(?P<name>[A-Za-z_]\w*)=\$\(if \[\[ "\$\{\{ needs\.determine-targets\.outputs\.fallback '
    r'\}\}" == "true" \]\]; then\s+echo "//\.\.\."\s+else\s+echo "\$\{\{ '
    r'needs\.determine-targets\.outputs\.affected \}\}"\s+fi\)')
# A shell assignment, quoted or bare, not part of a longer word or an option.
_ASSIGNMENT = re.compile(r'(?<![\w=-])([A-Za-z_][A-Za-z0-9_]*)=("(?:[^"\\]|\\.)*"|\S+)')
_WHOLE_VARIABLE = re.compile(r"^\$(?:\{([A-Za-z_]\w*)\}|([A-Za-z_]\w*))$")


def _resolve(value, environment, variables):
    """A script variable's value with the variables it reads substituted, as one string."""
    if len(value) >= 2 and value[0] == value[-1] == '"':
        value = value[1:-1]
    return _expand(value, {**environment, **variables})


def _expand_word(word, environment, variables):
    """Expand one shell word. A script variable stays one word; an env variable may split."""
    match = _WHOLE_VARIABLE.match(word)
    if match:
        name = match.group(1) or match.group(2)
        if name in variables:
            return [variables[name]]
        if name in environment:
            return environment[name].split()
    return [_expand(word, environment)]


def _invocation(line, environment, variables):
    """One `bazelisk build` or `bazelisk test` command line as option sets, or None."""
    match = re.search(r"(?:^|\s)(bazelisk|bazel)\s(.*)$", line)
    if not match:
        return None
    words = [expanded for word in _split_words(match.group(2))
             for expanded in _expand_word(word, environment, variables)]
    startup = []
    while words and words[0].startswith("--"):
        startup.append(words.pop(0))
    if not words or words[0] not in BAZEL_COMMANDS:
        return None
    command = words.pop(0)
    options = [word for word in words if word.startswith("--")]
    return {
        "command": command,
        "wrapped": "tools/ci_bazel_test.sh" in line[: match.start(1)],
        "startup": set(startup),
        "tag_filters": _tag_filters(options),
        "test_env": {option.split("=", 1)[1] for option in options
                     if option.startswith("--test_env=")},
        "flags": {option for option in options
                  if not option.startswith(("--test_env=", "--test_tag_filters="))},
        "targets": {word for word in words if not word.startswith("-")},
    }


_TEMPLATE = re.compile(r"\$\{\{(.*?)\}\}", re.DOTALL)


def render(text, scenario):
    """Evaluate each `${{ }}` expression in `text` for a scenario.

    An expression that reads a context the scenarios do not model (secrets, runner paths, the
    commit) is left as written, so it compares equal across scenarios.
    """
    def replace(match):
        try:
            value = _Expression(match.group(1), _context(scenario)).evaluate()
        except (ValueError, IndexError):
            return match.group(0)
        if isinstance(value, bool):
            return "true" if value else "false"
        return str(value)

    return _TEMPLATE.sub(replace, text)


def bazel_invocations(script, environment, scenario=None):
    """Every `bazelisk build` or `bazelisk test` command in a step script, as option sets.

    Script variables assigned before a command (such as the target selection) are resolved, so
    two steps that select targets differently compare as different. With a scenario, the
    script's other `${{ }}` expressions are evaluated for it.
    """
    script = _SELECTION.sub(lambda match: match.group("name") + "=" + SELECTED_TARGETS, script)
    if scenario is not None:
        script = render(script, scenario)
    script = _join_continuations(script)
    variables = {}
    invocations = []
    for line in script.splitlines():
        # A commented-out command runs nothing.
        line = _strip_comment(line)
        invocation = _invocation(line, environment, variables)
        if invocation:
            invocations.append(invocation)
            continue
        for name, value in _ASSIGNMENT.findall(line):
            variables[name] = _resolve(value, environment, variables)
    return invocations


def _tag_filters(options):
    filters = [option.split("=", 1)[1] for option in options
               if option.startswith("--test_tag_filters=")]
    if not filters:
        return None
    return {tag for tag in filters[-1].split(",") if tag}


def runtime_flag_writes(script):
    """`NAME=value` for each lane flag variable (`*_FLAGS`) a step rewrites through GITHUB_ENV.

    The value is the literal text up to the closing quote or end of line, so a step that writes
    different flags compares as different. A value built at run time (a Python writer joining
    strings) leaves only its literal prefix, which is empty.
    """
    if "GITHUB_ENV" not in script:
        return set()
    # Not `\b`: a Python writer spells the line break before the name as a literal "\n".
    return {"%s=%s" % (name, value.strip()) for name, value in
            re.findall(r"(?<![A-Z0-9_])([A-Z][A-Z0-9_]*_FLAGS)=([^\"\n]*)", script)}


# --- The comparison. -----------------------------------------------------------------------------


def _jobs(workflow):
    return workflow.get("jobs") or {}


def _environment(workflow, job):
    environment = dict(workflow.get("env") or {})
    environment.update(job.get("env") or {})
    return {key: str(value) for key, value in environment.items()}


def _continues_on_error(entry, scenario):
    """Whether a job's or step's `continue-on-error` is set for this scenario."""
    value = entry.get("continue-on-error")
    if value is None:
        return False
    if str(value).lower() in ("true", "false"):
        return str(value).lower() == "true"
    return runs(value, scenario)


# A status check function in a job's `if:` replaces GitHub's implicit `success()`, so the job can
# run even when a job it needs was skipped.
_STATUS_FUNCTION = re.compile(r"\b(?:always|failure|cancelled|success)\s*\(")


def _needs(job):
    needs = job.get("needs") or []
    return [needs] if isinstance(needs, str) else list(needs)


def job_runs(workflow, job_name, scenario):
    """Whether a job runs in this scenario, including the jobs it needs.

    Without a status check function in its `if:`, GitHub skips a job when any job it needs was
    skipped (failures are not modelled: every scenario assumes earlier jobs succeed).
    """
    job = _jobs(workflow)[job_name]
    condition = str(job.get("if") or "")
    if not runs(job.get("if"), scenario):
        return False
    if _STATUS_FUNCTION.search(condition) and "success(" not in condition:
        return True
    return all(job_runs(workflow, need, scenario) for need in _needs(job))


def job_gates(workflow, job_name, scenario):
    """Whether a job runs in this scenario and its failure fails the run."""
    return (job_runs(workflow, job_name, scenario)
            and not _continues_on_error(_jobs(workflow)[job_name], scenario))


def _gating_steps(job, scenario):
    """Steps that check something and can fail the job in this scenario.

    Under a job-level `continue-on-error`, no step can fail the run. A step-level one exempts
    that step. A cache save checks nothing: it records state for later runs, and only main
    pushes may write caches.
    """
    if _continues_on_error(job, scenario):
        return
    for step in job.get("steps") or []:
        if _continues_on_error(step, scenario):
            continue
        if str(step.get("uses") or "").startswith("actions/cache/save@"):
            continue
        yield step


def _step_name(step):
    """A step's name or id; otherwise its action and inputs, or the first line of its script."""
    if step.get("name") or step.get("id"):
        return step.get("name") or step.get("id")
    if step.get("uses"):
        return "%s %s" % (step["uses"], sorted((step.get("with") or {}).items()))
    return "run: " + str(step.get("run") or "").strip().split("\n", 1)[0]


def _step_environment(workflow, job, step, scenario=None):
    """The environment a step's script sees: workflow, then job, then step `env:`.

    With a scenario, `${{ }}` expressions in the values are evaluated for it.
    """
    environment = _environment(workflow, job)
    environment.update({key: str(value) for key, value in (step.get("env") or {}).items()})
    if scenario is not None:
        environment = {key: render(value, scenario) for key, value in environment.items()}
    return environment


def _bazel_profile(workflow, job, scenario):
    """The Bazel invocations of each gating step that runs, and the flag variables rewritten."""
    steps = {}
    writes = set()
    for step in _gating_steps(job, scenario):
        if not runs(step.get("if"), scenario):
            continue
        script = str(step.get("run") or "")
        writes |= runtime_flag_writes(script)
        invocations = bazel_invocations(script, _step_environment(workflow, job, step, scenario),
                                        scenario)
        if invocations:
            steps[_step_name(step)] = invocations
    return steps, writes


def _other_step_view(workflow, job, step, scenario):
    """What a non-Bazel step runs in a scenario: action, inputs, command, and the variables read.

    `${{ }}` expressions are rendered for the scenario, so a check that is stricter on one event
    than another compares as different. Each variable the command or inputs read is recorded
    with its effective value, wherever it is set.
    """
    script = render(str(step.get("run") or ""), scenario)
    environment = _step_environment(workflow, job, step, scenario)
    # Inputs that differ between events by policy, for the action that reads them, are not
    # compared (_POLICY_EVENT_INPUTS).
    action = str(step.get("uses") or "").split("@", 1)[0]
    inputs = sorted((key, render(str(value), scenario))
                    for key, value in (step.get("with") or {}).items()
                    if (action, key) not in _POLICY_EVENT_INPUTS)
    read = (set(re.findall(r"\$\{?([A-Za-z_][A-Za-z0-9_]*)", script))
            | set(re.findall(r"\benv\.([A-Za-z_][A-Za-z0-9_]*)", str(inputs))))
    return (step.get("uses"), inputs, script,
            sorted((name, environment.get(name)) for name in read))


def _other_steps(workflow, job, scenario):
    """Gating steps that run in this scenario and run no Bazel build or test, by name."""
    other = {}
    for step in _gating_steps(job, scenario):
        if runs(step.get("if"), scenario) and not bazel_invocations(
                str(step.get("run") or ""), _step_environment(workflow, job, step)):
            other[_step_name(step)] = _other_step_view(workflow, job, step, scenario)
    return other


def _other_step_differences(workflow, main_job, pr_job):
    """Non-Bazel checks main's job runs that the pull request job lacks or runs differently."""
    main_other = _other_steps(workflow, main_job, "main-push")
    pr_other = _other_steps(workflow, pr_job, "trusted-pr")
    return ({(name, "step-main-only", "") for name in set(main_other) - set(pr_other)}
            | {(name, "step-changed", "") for name in set(main_other) & set(pr_other)
               if main_other[name] != pr_other[name]})


def _set_differences(name, kind, main_values, pr_values):
    return ({(name, kind + "-main-only", value) for value in main_values - pr_values}
            | {(name, kind + "-pr-only", value) for value in pr_values - main_values})


def _run_differences(name, main_run, pr_run):
    differences = set()
    if main_run["wrapped"] != pr_run["wrapped"]:
        differences.add((name, "exit-status-wrapper", ""))
    if (main_run["tag_filters"] is None) != (pr_run["tag_filters"] is None):
        differences.add((name, "tag-filter-presence", ""))
    for kind, key in (("startup", "startup"), ("flag", "flags"), ("test-env", "test_env"),
                      ("tag-filter", "tag_filters"), ("target", "targets")):
        differences |= _set_differences(name, kind, main_run[key] or set(),
                                        pr_run[key] or set())
    return differences


def _step_differences(name, main_runs, pr_runs):
    if [run["command"] for run in main_runs] != [run["command"] for run in pr_runs]:
        return {(name, "commands", "")}
    differences = set()
    for main_run, pr_run in zip(main_runs, pr_runs):
        differences |= _run_differences(name, main_run, pr_run)
    return differences


def _compared_steps(main_steps, pr_steps, only_steps):
    """Differences in just `only_steps`, each of which both jobs must run."""
    differences = {(name, "step-missing", "") for name in only_steps
                   if name not in main_steps or name not in pr_steps}
    shared = {name for name in only_steps if name in main_steps and name in pr_steps}
    for name in shared:
        differences |= _step_differences(name, main_steps[name], pr_steps[name])
    return _fold_shared(differences, shared)


def invocation_differences(workflow, main_job_name, replacement_name, only_steps=None):
    """(step, kind, value) differences between a main job and the job that replaces it.

    With `only_steps`, compare just those steps, each of which both jobs must run.
    """
    jobs = _jobs(workflow)
    main_steps, main_writes = _bazel_profile(workflow, jobs[main_job_name], "main-push")
    pr_steps, pr_writes = _bazel_profile(workflow, jobs[replacement_name], "trusted-pr")
    if only_steps is not None:
        return _compared_steps(main_steps, pr_steps, only_steps)
    differences = _set_differences("*", "runtime-env", main_writes, pr_writes)
    differences |= {(name, "step-main-only", "") for name in set(main_steps) - set(pr_steps)}
    differences |= _other_step_differences(workflow, jobs[main_job_name], jobs[replacement_name])
    shared = set(main_steps) & set(pr_steps)
    for name in shared:
        differences |= _step_differences(name, main_steps[name], pr_steps[name])
    return _fold_shared(differences, shared)


def _setup_view(step):
    """A setup step's identity: condition, what it runs, and its inputs as a sorted list."""
    inputs = dict(step.get("with") or {})
    for step_name, key in SUBSET_SETUP_REQUIRED_INPUTS:
        if _step_name(step) == step_name:
            inputs.pop(key, None)
    return (step.get("if"), step.get("uses"), step.get("run"), sorted(inputs.items()))


def _required_input_problems(name, step):
    return [(name, "%s is not %s" % (key, value))
            for (step_name, key), value in SUBSET_SETUP_REQUIRED_INPUTS.items()
            if step_name == name and str((step.get("with") or {}).get(key)) != value]


def subset_setup_differences(workflow, main_job_name, subset_name, covered):
    """Setup steps that main's job runs before a covered step and the subset lacks or changes.

    Setup is every gating step before the first covered step that runs no Bazel build or test:
    checkout, toolchain installs, cache restores and dependency fetches. Bazel builds and tests
    there are the work the subset deliberately skips, and its Bazel steps are compared
    separately.
    """
    jobs = _jobs(workflow)
    main_job, subset = jobs[main_job_name], jobs[subset_name]
    environment = _environment(workflow, main_job)
    subset_steps = {_step_name(step): step for step in _gating_steps(subset, "trusted-pr")}
    # The job-level environment binding decides which cache key the setup steps receive.
    problems = ([] if subset.get("environment") == main_job.get("environment")
                else [("environment", "changed")])
    for step in _gating_steps(main_job, "main-push"):
        name = _step_name(step)
        if name in covered:
            break
        if bazel_invocations(str(step.get("run") or ""), environment):
            continue
        if name not in subset_steps:
            problems.append((name, "missing"))
            continue
        if _setup_view(step) != _setup_view(subset_steps[name]):
            problems.append((name, "changed"))
        problems.extend(_required_input_problems(name, subset_steps[name]))
    return problems + subset_order_problems(main_job, subset)


def subset_order_problems(main_job, subset):
    """Subset steps that main's job does not run, or runs in a different order.

    The subset's gating steps, in order, must be a subsequence of main's: its setup steps (so
    BuildBuddy and toolchain setup still precede every Bazel step), the Bazel steps it keeps, the
    covered steps and any later failure handling. A step main's job does not have could change
    what the covered steps test (for example, by writing a bazelrc), so it needs a reviewed
    entry in SUBSET_EXTRA_STEPS.
    """
    main_names = [_step_name(step) for step in _gating_steps(main_job, "main-push")]
    problems = []
    position = -1
    for step in _gating_steps(subset, "trusted-pr"):
        name = _step_name(step)
        if name not in main_names:
            if name not in SUBSET_EXTRA_STEPS:
                problems.append((name, "not in main's job"))
            continue
        index = main_names.index(name)
        if index <= position:
            problems.append((name, "out of order"))
        position = max(position, index)
    return problems


def subset_differences(workflow, main_job_name, subset_name, covered):
    """How a hosted subset job's Bazel invocations differ from the main job's.

    Every Bazel step the subset runs must be a main step with the same invocation, every covered
    step must be among them, and both jobs must rewrite the same flag variables at run time.
    """
    jobs = _jobs(workflow)
    main_steps, main_writes = _bazel_profile(workflow, jobs[main_job_name], "main-push")
    subset_steps, subset_writes = _bazel_profile(workflow, jobs[subset_name], "trusted-pr")
    differences = _set_differences("*", "runtime-env", main_writes, subset_writes)
    differences |= {(name, "step-missing", "") for name in set(covered) - set(subset_steps)}
    differences |= {(name, "step-subset-only", "") for name in set(subset_steps) - set(main_steps)}
    for name in set(subset_steps) & set(main_steps):
        differences |= _step_differences(name, main_steps[name], subset_steps[name])
    return differences


def _fold_shared(differences, shared_steps):
    """Report a flag difference once under "*" when every shared step has it."""
    folded = set()
    by_value = {}
    for step, kind, value in differences:
        by_value.setdefault((kind, value), set()).add(step)
    for (kind, value), steps in by_value.items():
        if kind.startswith(("startup-", "flag-")) and shared_steps and steps >= shared_steps:
            folded.add(("*", kind, value))
        else:
            folded.update((step, kind, value) for step in steps)
    return folded


def _branch_pattern(pattern):
    """A GitHub branch filter pattern (`*`, `**`, `?`) as an anchored regular expression."""
    parts = re.split(r"(\*\*|\*|\?)", pattern)
    wildcards = {"**": ".*", "*": "[^/]*", "?": "."}
    return re.compile("".join(wildcards.get(part, re.escape(part)) for part in parts) + r"\Z")


def branch_filter_includes(branch, branches=None, ignore=None):
    """Whether a `branches` or `branches-ignore` filter lets an event on `branch` run.

    `branches` patterns apply in order, and a later `!pattern` excludes what an earlier pattern
    included; any matching `branches-ignore` pattern excludes the branch.
    """
    if ignore is not None:
        return not any(_branch_pattern(pattern).match(branch) for pattern in ignore)
    if branches is None:
        return True
    included = False
    for pattern in branches:
        negated = pattern.startswith("!")
        if _branch_pattern(pattern[1:] if negated else pattern).match(branch):
            included = not negated
    return included


def triggers(trigger):
    """A workflow's `on:` as a mapping, expanding `on: push` and `on: [push, pull_request]`."""
    if isinstance(trigger, str):
        return {trigger: None}
    if isinstance(trigger, list):
        return {event: None for event in trigger}
    return trigger if isinstance(trigger, dict) else {}


# Pull request activity types that cover every new revision of an open pull request.
_REVISION_TYPES = {"opened", "synchronize", "reopened"}


def _push_runs_on_main(trigger):
    trigger = triggers(trigger)
    if "push" not in trigger:
        return False
    push = trigger["push"] or {}
    # A push trigger that filters only tags never runs for branch pushes.
    if push.get("branches") is None and push.get("branches-ignore") is None and (
            "tags" in push or "tags-ignore" in push):
        return False
    return branch_filter_includes("main", push.get("branches"), push.get("branches-ignore"))


def _pull_request_trigger_differences(name, trigger):
    trigger = triggers(trigger)
    if "pull_request" not in trigger:
        return {(name, "push-only")}
    observed = set()
    push_paths = (trigger["push"] or {}).get("paths")
    pull_request = trigger["pull_request"] or {}
    pr_paths = pull_request.get("paths")
    if pr_paths is not None and pr_paths != push_paths:
        observed.add((name, "pull-request-paths"))
    if not branch_filter_includes("main", pull_request.get("branches"),
                                  pull_request.get("branches-ignore")):
        observed.add((name, "pull-request-branches"))
    if pull_request.get("paths-ignore") is not None:
        observed.add((name, "pull-request-paths-ignore"))
    types = pull_request.get("types")
    if types is not None and not _REVISION_TYPES <= set(types):
        observed.add((name, "pull-request-types"))
    return observed


def trigger_differences(workflows, main_push_workflows):
    """How each main-push workflow's pull request trigger is narrower than its push trigger."""
    observed = set()
    for name in main_push_workflows:
        observed |= _pull_request_trigger_differences(name, workflows[name]["on"])
    return observed


def _jobs_on_both_triggers(workflows, main_push_workflows):
    for name in main_push_workflows:
        workflow = workflows[name]
        if "pull_request" in triggers(workflow.get("on")):
            for job_name, job in _jobs(workflow).items():
                yield name, job_name, job


def skipped_main_jobs(workflows, main_push_workflows):
    """Jobs main runs that no pull request runs, and (job, scenario) pairs one kind skips."""
    main_only = set()
    skipped = set()
    for name, job_name, job in _jobs_on_both_triggers(workflows, main_push_workflows):
        workflow = workflows[name]
        if not job_gates(workflow, job_name, "main-push"):
            continue
        missing = [scenario for scenario in PULL_REQUEST_SCENARIOS
                   if not job_gates(workflow, job_name, scenario)]
        if len(missing) == len(PULL_REQUEST_SCENARIOS):
            main_only.add((name, job_name))
        else:
            skipped.update((name, job_name, scenario) for scenario in missing)
    return main_only, skipped


def _running_steps(job, scenario):
    """Names of the gating steps that run in this scenario."""
    return {_step_name(step) for step in _gating_steps(job, scenario)
            if runs(step.get("if"), scenario)}


def _scenario_differences(workflow, job, scenario):
    """How one shared job's rendered steps differ between a main push and a scenario."""
    main_steps, _ = _bazel_profile(workflow, job, "main-push")
    pr_steps, _ = _bazel_profile(workflow, job, scenario)
    # A step that runs in both scenarios but runs Bazel only on main (its command rendered away
    # on pull requests). Steps the pull request skips outright are reported by
    # skipped_main_steps.
    differences = {(step, "step-main-only", "") for step in set(main_steps) - set(pr_steps)
                   if step in _running_steps(job, scenario)}
    for step in set(main_steps) & set(pr_steps):
        differences |= _step_differences(step, main_steps[step], pr_steps[step])
    main_other = _other_steps(workflow, job, "main-push")
    pr_other = _other_steps(workflow, job, scenario)
    return differences | {(step, "step-changed", definition_digest(main_other[step], pr_other[step]))
                          for step in set(main_other) & set(pr_other)
                          if main_other[step] != pr_other[step]}


def definition_digest(main_view, pr_view):
    """A stable digest of a step's rendered definitions on main and on a pull request."""
    text = json.dumps([main_view, pr_view], sort_keys=True)
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:16]


def shared_job_differences(workflows, main_push_workflows):
    """How a job that runs on both main and a pull request runs differently on each.

    A shared step's script, inputs or environment can branch on the event through `${{ }}`
    expressions, so each scenario's rendered Bazel invocations and other checks are compared
    step by step.
    """
    observed = set()
    for name, job_name, job in _jobs_on_both_triggers(workflows, main_push_workflows):
        workflow = workflows[name]
        if not job_gates(workflow, job_name, "main-push"):
            continue
        for scenario in PULL_REQUEST_SCENARIOS:
            if job_gates(workflow, job_name, scenario):
                observed.update((name, job_name, scenario) + difference for difference in
                                _scenario_differences(workflow, job, scenario))
    return observed


def skipped_main_steps(workflows, main_push_workflows):
    """Gating steps main runs that a pull request skips while running the same job."""
    observed = set()
    for name, job_name, job in _jobs_on_both_triggers(workflows, main_push_workflows):
        if not job_gates(workflows[name], job_name, "main-push"):
            continue
        main_steps = _running_steps(job, "main-push")
        for scenario in PULL_REQUEST_SCENARIOS:
            if job_gates(workflows[name], job_name, scenario):
                observed.update((name, job_name, step, scenario)
                                for step in main_steps - _running_steps(job, scenario))
    return observed


# A Bazel build or test command, after any startup options.
_BAZEL_COMMAND = r"(?:bazelisk|bazel)\s+(?:--\S+\s+)*(?:build|test)\b"
_BAZEL_RUN = re.compile(r"(?:^|[\s;&|(!])" + _BAZEL_COMMAND)
# Pattern-based checks of a Bazel-running step's script for constructs that can turn a failure
# into success. Workflow authors are trusted: these guard against accidental drift and are not
# exhaustive.
_NEGATED_BAZEL = re.compile(r"!\s*[^;&|]*?(?:^|[\s/\"'])" + _BAZEL_COMMAND)
# A Bazel command used as the condition of `if`, `elif`, `while` or `until`, which exempts it from
# errexit.
_CONDITION_BAZEL = re.compile(r"\b(?:if|elif|while|until)\s+(?:!\s*)?(?:[^;]*?[\s/\"'])?"
                              + _BAZEL_COMMAND)
_EXIT = re.compile(r"(?<![\w$.-])exit\b(?![\w=-])\s*([^\s;&|)]*)")
_SET = re.compile(r"(?:^|[;&|]\s*|\b(?:then|do|else)\s+)set(?:\s+([^;&|]*))?")
_SET_OPTION = re.compile(r"^[-+][A-Za-z]+$")
_SET_OPTION_NAMES = {"allexport", "braceexpand", "emacs", "errexit", "errtrace", "functrace",
                     "hashall", "histexpand", "history", "ignoreeof", "keyword", "monitor",
                     "noclobber", "noexec", "noglob", "nolog", "notify", "nounset", "onecmd",
                     "physical", "pipefail", "posix", "privileged", "verbose", "vi", "xtrace"}


def _set_option_masks(word):
    """Whether one `set` word is not a literal option group, or turns errexit off (`+e`)."""
    return not _SET_OPTION.match(word) or (word.startswith("+") and "e" in word[1:])


def _set_masks(arguments):
    """Whether a `set` command's arguments are other than literal options, or disable errexit.

    An option group ending in `o` (`-o`, `-euo`, `+o`) takes the next word as an option name.
    """
    words = arguments.split()
    index = 0
    while index < len(words):
        word = words[index]
        if _set_option_masks(word):
            return True
        if word.endswith("o"):
            index += 1
            name = words[index] if index < len(words) else ""
            if name not in _SET_OPTION_NAMES or (word.startswith("+") and name == "errexit"):
                return True
        index += 1
    return False


def _line_masks(line):
    """Whether one logical line can turn a failure into success."""
    bazel = _BAZEL_RUN.search(line)
    if bazel and re.search(r"&&|\|\|", line[bazel.start():]):
        return True
    if _NEGATED_BAZEL.search(line) or _CONDITION_BAZEL.search(line):
        return True
    if re.search(r"\|\|\s*(?:true|:)\s*$", line):
        return True
    for match in _EXIT.finditer(line):
        if not re.fullmatch(r"[1-9][0-9]*", match.group(1)):
            return True
    if any(_set_masks(match.group(1) or "") for match in _SET.finditer(line)):
        return True
    return bool(re.search(r"\btrap\b", line) and re.search(r"(?<![\w$.-])exit\b", line))


def _close_group(open_groups, rest):
    """Close the innermost group; whether it held a Bazel command and `rest` uses && or ||."""
    contained = open_groups.pop()
    if contained and open_groups:
        open_groups[-1] = True
    return bool(contained and re.search(r"^[^(]*?(?:&&|\|\|)", rest))


def _scan_groups(line, open_groups):
    """Track `(` groups across lines; whether this line closes a Bazel group into && or ||."""
    bazel = _BAZEL_RUN.search(line)
    bazel_at = bazel.start() if bazel else -1
    masked = False
    for index, char in enumerate(line):
        if char == "(":
            open_groups.append(False)
        elif char == ")" and open_groups:
            masked = _close_group(open_groups, line[index + 1:]) or masked
        if index == bazel_at:
            open_groups[:] = [True] * len(open_groups)
    return masked


def _group_masks(lines):
    """Lines that close a `(` or `$(` group containing a Bazel command, then use && or ||."""
    open_groups = []
    return {line for line in lines if _scan_groups(line, open_groups)}


def failure_masks(script, shell=None):
    """Each line of a Bazel-running step that can mask a failure, after joining continuations.

    Pattern-based; it guards against accidental drift and is not exhaustive. It flags a `shell:`
    other than unset or `bash`; `!` before a Bazel command; `&&` or `||` after a Bazel command,
    or after a group that contains one; any `exit` whose argument is not a literal non-zero
    integer; any `set` whose arguments are not all literal options, or that disables errexit;
    a `trap` that runs `exit`; and any line ending in `|| true` or `|| :`.
    """
    found = set()
    if shell not in (None, "bash"):
        found.add("shell: %s" % shell)
    # A `${{ }}` expression is evaluated by GitHub before the shell runs, so its operators are
    # not shell operators.
    script = _TEMPLATE.sub("EXPR", script)
    lines = [" ".join(_strip_comment(line).split())
             for line in _join_continuations(script).splitlines()]
    lines = [line for line in lines if line]
    found.update(line for line in lines if _line_masks(line))
    return found | _group_masks(lines)


def failure_swallowing_lines(workflows, names):
    """(workflow, job, step, line) for each Bazel-running gating step line that can mask failure.

    A Bazel command compared by this test proves nothing if its step then ignores the exit
    status, so such lines must be reviewed and allowlisted.
    """
    found = set()
    for name, job_name, job in _jobs_on_both_triggers(workflows, names):
        for step in _bazel_running_steps(workflows[name], job):
            found.update((name, job_name, _step_name(step), line)
                         for line in failure_masks(str(step["run"]), step.get("shell")))
    return found


def _bazel_running_steps(workflow, job):
    """Gating steps that run a Bazel build or test in any scenario."""
    seen = {}
    for scenario in ("main-push",) + PULL_REQUEST_SCENARIOS:
        for step in _gating_steps(job, scenario):
            script = str(step.get("run") or "")
            if runs(step.get("if"), scenario) and bazel_invocations(
                    script, _step_environment(workflow, job, step)):
                seen[id(step)] = step
    return list(seen.values())


class CiLaneParityTest(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls):
        cls.workflows = {}
        for path in _workflow_paths():
            cls.workflows[path.name] = parse_workflow(path.read_text(encoding="utf-8"))
        cls.main_push_workflows = sorted(
            name for name, workflow in cls.workflows.items()
            if _push_runs_on_main(workflow.get("on")))

    def test_reader_understands_the_main_workflow(self):
        jobs = _jobs(self.workflows["main.yml"])
        for name in ("gatekeeper", "determine-targets", "linux", "linux-self-hosted", "macos",
                     "macos-self-hosted"):
            self.assertIn(name, jobs)
        steps = [_step_name(step) for step in jobs["macos"]["steps"]]
        self.assertIn("Test", steps)
        self.assertIn("Test (browser targets)", steps)
        self.assertIn("HOSTED_MACOS_QUARANTINE_TEST_FLAGS", jobs["macos"]["env"])
        test = next(step for step in jobs["macos"]["steps"] if _step_name(step) == "Test")
        self.assertIn("bazelisk test", test["run"])
        self.assertIn("main.yml", self.main_push_workflows)

    def test_expressions_follow_the_routing_gate(self):
        jobs = _jobs(self.workflows["main.yml"])
        expected = {
            "macos": {"main-push": True, "trusted-pr": False, "untrusted-pr": True},
            "macos-self-hosted": {"main-push": False, "trusted-pr": True, "untrusted-pr": False},
            "linux": {"main-push": True, "trusted-pr": True, "untrusted-pr": True},
            "linux-self-hosted": {"main-push": False, "trusted-pr": True, "untrusted-pr": False},
        }
        for job, scenarios in expected.items():
            for scenario, expected_runs in scenarios.items():
                with self.subTest(job=job, scenario=scenario):
                    self.assertEqual(runs(jobs[job].get("if"), scenario), expected_runs)
        self.assertFalse(runs("${{ !cancelled() && failure() }}", "main-push"))
        self.assertTrue(runs("${{ github.event_name == 'push' || false }}", "main-push"))
        with self.assertRaises(ValueError):
            runs("${{ github.unknown_context == 'x' }}", "main-push")

    def test_every_main_push_workflow_runs_on_pull_requests(self):
        self._assert_matches_allowlist(
            trigger_differences(self.workflows, self.main_push_workflows), TRIGGER_ALLOWLIST,
            "main runs these workflows but pull requests do not; run them on pull requests "
            "with the same filters or allowlist them with a reason")

    def _assert_matches_allowlist(self, observed, allowlist, message):
        self.assertEqual(sorted(observed - set(allowlist)), [], message)
        self.assertEqual(sorted(set(allowlist) - observed), [], "stale allowlist entries")

    def test_every_job_a_main_push_runs_also_runs_on_pull_requests(self):
        main_only, skipped = skipped_main_jobs(self.workflows, self.main_push_workflows)
        self._assert_matches_allowlist(main_only, MAIN_ONLY_JOBS,
                                       "jobs main runs that no pull request runs")
        self._assert_matches_allowlist(
            skipped, JOB_REPLACEMENTS,
            "jobs main runs that a kind of pull request skips without a replacement")
        for (name, job_name, scenario), replacement in JOB_REPLACEMENTS.items():
            self.assertTrue(job_gates(self.workflows[name], replacement, scenario),
                            "%s does not gate a %s" % (replacement, scenario))

    def test_every_step_a_main_push_runs_also_runs_on_pull_requests(self):
        self._assert_matches_allowlist(
            skipped_main_steps(self.workflows, self.main_push_workflows), STEP_ALLOWLIST,
            "steps main runs that a pull request skips in the same job")

    def test_replacement_jobs_run_the_same_bazel_invocations(self):
        observed = set()
        # A step a hosted subset covers is still compared with the replacement's: the
        # replacement keeps running it on every trusted PR, including those the subset skips.
        for (name, job_name, _), replacement in JOB_REPLACEMENTS.items():
            workflow = self.workflows[name]
            for step, kind, value in invocation_differences(workflow, job_name, replacement):
                observed.add((name, job_name, replacement, step, kind, value))
        self._assert_matches_allowlist(
            observed, INVOCATION_ALLOWLIST,
            "the pull request lane runs Bazel differently from main's lane")

    def test_counterpart_steps_run_the_same_bazel_invocations(self):
        observed = set()
        for (name, job_name, scenario), (counterpart, steps) in COUNTERPART_STEPS.items():
            with self.subTest(counterpart=counterpart):
                for step in steps:
                    # A counterpart stands in for a main step the pull request skips.
                    self.assertIn((name, job_name, step, scenario), STEP_ALLOWLIST)
                self.assertTrue(job_gates(self.workflows[name], counterpart, scenario))
            workflow = self.workflows[name]
            for step, kind, value in invocation_differences(workflow, job_name, counterpart,
                                                            only_steps=steps):
                observed.add((name, job_name, counterpart, step, kind, value))
        self._assert_matches_allowlist(
            observed, COUNTERPART_ALLOWLIST,
            "the pull request runs a skipped main step differently in its counterpart job")

    def test_target_selection_differences_are_reported(self):
        selection = ('TARGETS=$(if [[ "${{ needs.determine-targets.outputs.fallback }}" == '
                     '"true" ]]; then\n            echo "//..."\n          else\n'
                     '            echo "${{ needs.determine-targets.outputs.affected }}"\n'
                     '          fi)\n          ')
        main = textwrap_job('X_TARGETS: "//tools/ci:a"',
                            selection + "bazelisk test --config=ci $TARGETS $X_TARGETS")
        narrowed = textwrap_job('X_TARGETS: "//tools/ci:b"',
                                selection + "bazelisk test --config=ci //donner/base/... "
                                "$X_TARGETS")
        workflow = parse_workflow("jobs:\n  main:\n" + main + "  pr:\n" + narrowed)
        self.assertEqual(invocation_differences(workflow, "main", "pr"), {
            ("Test", "target-main-only", SELECTED_TARGETS),
            ("Test", "target-pr-only", "//donner/base/..."),
            ("Test", "target-main-only", "//tools/ci:a"),
            ("Test", "target-pr-only", "//tools/ci:b"),
        })
        same = textwrap_job('X_TARGETS: "//tools/ci:a"',
                            selection + "bazelisk test --config=ci $TARGETS $X_TARGETS")
        workflow = parse_workflow("jobs:\n  main:\n" + main + "  pr:\n" + same)
        self.assertEqual(invocation_differences(workflow, "main", "pr"), set())

    def test_reader_rejects_what_it_cannot_parse(self):
        with self.assertRaisesRegex(ValueError, "unsupported YAML"):
            parse_workflow("jobs:\n  a:\n    env:\n      X: one\n        two\n"
                           "  b:\n    runs-on: x\n")
        self.assertEqual(parse_workflow("a:\n  b: c\n# trailing comment\n\n"),
                         {"a": {"b": "c"}})

    def test_job_level_continue_on_error_makes_a_job_non_gating(self):
        job = {"if": "${{ github.event_name == 'pull_request' }}", "continue-on-error": "true",
               "steps": [{"name": "Test", "run": "bazelisk test //..."}]}
        self.assertFalse(job_gates({"jobs": {"x": job}}, "x", "trusted-pr"))
        self.assertEqual(list(_gating_steps(job, "trusted-pr")), [])
        job["continue-on-error"] = "${{ github.event_name == 'push' }}"
        self.assertTrue(job_gates({"jobs": {"x": job}}, "x", "trusted-pr"))
        self.assertEqual(len(list(_gating_steps(job, "trusted-pr"))), 1)

    def test_bazel_steps_do_not_swallow_failure(self):
        self._assert_matches_allowlist(
            failure_swallowing_lines(self.workflows, self.main_push_workflows),
            FAILURE_SWALLOW_ALLOWLIST,
            "a step that runs Bazel can turn its failure into success")

    def test_failure_swallowing_lines_are_found(self):
        workflow = parse_workflow(
            "on:\n  push:\n  pull_request:\n"
            "jobs:\n  build:\n    steps:\n"
            "      - name: Test\n        run: |\n          set +e\n"
            "          bazelisk test //...\n          exit 0\n"
            "      - name: Fetch\n        run: |\n          bazelisk fetch //... || true\n")
        self.assertEqual(failure_swallowing_lines({"x.yml": workflow}, ["x.yml"]), {
            ("x.yml", "build", "Test", "set +e"),
            ("x.yml", "build", "Test", "exit 0"),
        })
        masking = (
            "bazelisk test //... || exit 0",
            "tools/ci_bazel_test.sh bazelisk --nohome_rc build --config=ci \\\n  //a:b \\\n"
            "  || echo skipped",
            "set +o errexit",
            "set +eu",
            "set -u +e",
            "set -o pipefail +e",
            "  exit 0  # done",
            "cleanup || true",
            "cleanup || :",
            "bazelisk test //... && echo passed",
            "if ! bazelisk test //...; then echo failed; fi",
            "! tools/ci_bazel_test.sh bazelisk test //...",
            "exit $((0))",
            "exit \"$x\"",
            "exit",
            "set $relax",
            "set -o \"$mode\"",
            "trap 'exit 0' ERR",
            "if bazelisk test //...; then echo passed; fi",
            "elif tools/ci_bazel_test.sh bazelisk test //a; then echo passed",
            "while bazel build //a; do break; done",
            "until bazelisk --nohome_rc test //a; do echo retry; done",
        )
        for script in masking:
            with self.subTest(script=script):
                self.assertEqual(len(failure_masks(script)), 1, script)
        # A group holding a Bazel command, closed and then followed by || or &&.
        self.assertEqual(failure_masks("out=$(\n  bazelisk test //...\n) || echo failed"),
                         {") || echo failed"})
        self.assertEqual(failure_masks("(\n  cd x\n  bazelisk build //a\n) && echo built"),
                         {") && echo built"})
        self.assertEqual(failure_masks("bazelisk test //...", "bash --noprofile --norc {0}"),
                         {"shell: bash --noprofile --norc {0}"})
        for script in ("set -euo pipefail", "set +x", "set -x", "bazelisk fetch //a:b || retry",
                       "bazelisk test ${{ github.event_name == 'push' && '//...' || '//a' }}",
                       "[[ -n \"$a\" || -n \"$b\" ]]", "exit 1", "exit 4  # mapped",
                       "[[ -n \"$a\" ]] && bazelisk test //a", "trap cleanup EXIT",
                       "# exit status is mapped below", "x=$(python3 a.py) || exit 1"):
            with self.subTest(script=script):
                self.assertEqual(failure_masks(script), set(), script)
        for shell in (None, "bash"):
            self.assertEqual(failure_masks("bazelisk test //...", shell), set())

    def test_step_names_are_unique_within_each_job(self):
        # Steps are compared by name, so a second step with a compared step's name could hide
        # a main-only check behind the step that matches.
        for name, workflow in self.workflows.items():
            for job_name, job in _jobs(workflow).items():
                names = [_step_name(step) for step in job.get("steps") or []]
                duplicates = sorted({step for step in names if names.count(step) > 1})
                self.assertEqual(duplicates, [], "%s:%s" % (name, job_name))

    def test_every_modelled_step_output_is_read(self):
        read = set()
        for name in self.main_push_workflows:
            for job in _jobs(self.workflows[name]).values():
                for entry in [job] + list(job.get("steps") or []):
                    for key in ("if", "continue-on-error"):
                        read.update(re.findall(r"steps\.[A-Za-z0-9_-]+\.outputs\.[A-Za-z0-9_-]+",
                                               str(entry.get(key) or "")))
        self.assertEqual(sorted(set(STEP_OUTPUTS) - read), [], "stale STEP_OUTPUTS entries")

    def test_step_environment_overrides_the_job_environment(self):
        main = textwrap_job('X_FLAGS: "--test_env=A=1"', "bazelisk test $X_FLAGS //...")
        overridden = textwrap_job('X_FLAGS: "--test_env=A=1"', "bazelisk test $X_FLAGS //...")
        overridden = overridden.replace(
            "        run: |", "        env:\n          X_FLAGS: --test_filter=NoSuchCase\n"
            "        run: |")
        workflow = parse_workflow("jobs:\n  main:\n" + main + "  pr:\n" + overridden)
        self.assertEqual(invocation_differences(workflow, "main", "pr"), {
            ("Test", "test-env-main-only", "A=1"),
            ("*", "flag-pr-only", "--test_filter=NoSuchCase"),
        })

    def test_runtime_rewrites_compare_their_values(self):
        self.assertEqual(runtime_flag_writes(
            'echo "BAZEL_X_FLAGS=--a --b" >> "$GITHUB_ENV"\n'), {"BAZEL_X_FLAGS=--a --b"})
        self.assertEqual(runtime_flag_writes('echo "BAZEL_X_FLAGS=--a"\n'), set())
        self.assertEqual(runtime_flag_writes(
            'output.write("\\nBAZEL_X_FLAGS=" + flags)  # GITHUB_ENV\n'), {"BAZEL_X_FLAGS="})
        # Every lane flag variable counts, not only BAZEL_*.
        self.assertEqual(runtime_flag_writes(
            'echo "HOSTED_MACOS_QUARANTINE_TEST_FLAGS=" >> "$GITHUB_ENV"\n'
            'echo "BROWSER_STALL_TEST_FLAGS=--test_env=X=1" >> "$GITHUB_ENV"\n'),
            {"HOSTED_MACOS_QUARANTINE_TEST_FLAGS=", "BROWSER_STALL_TEST_FLAGS=--test_env=X=1"})

    def test_unmodelled_job_outputs_raise(self):
        with self.assertRaisesRegex(ValueError, "unmodelled context"):
            runs("${{ needs.gatekeeper.outputs.some_new_output == 'true' }}", "trusted-pr")
        with self.assertRaisesRegex(ValueError, "unmodelled context"):
            runs("${{ steps.test.outputs.main_only == 'true' }}", "main-push")
        self.assertFalse(runs("${{ steps.base-hashes.outputs.cache-hit == 'true' }}",
                              "main-push"))
        self.assertTrue(runs("${{ steps.pr-meta.outputs.base_sha != '' }}", "trusted-pr"))
        self.assertEqual(runs("${{ needs.gatekeeper.outputs.trusted_pr == 'true' }}",
                              "trusted-pr"), True)
        self.assertEqual(runs("${{ needs.trust.outputs.renovate_pr == 'true' }}",
                              "renovate-pr"), True)
        self.assertEqual(runs("${{ needs.gatekeeper.outputs.trusted_pr == 'true' }}",
                              "main-push"), False)

    def test_pull_request_branch_filters_must_include_main(self):
        workflows = {
            "release.yml": parse_workflow("on:\n  push:\n    branches: [main]\n"
                                          "  pull_request:\n    branches: [release]\n"),
            "all.yml": parse_workflow("on:\n  push:\n    branches: [main]\n"
                                      "  pull_request:\n    branches: [main, release]\n"),
        }
        self.assertEqual(trigger_differences(workflows, sorted(workflows)),
                         {("release.yml", "pull-request-branches")})

    def test_jobs_skip_when_a_needed_job_is_skipped(self):
        workflow = parse_workflow(
            "jobs:\n  prepare:\n    if: github.event_name == 'push'\n"
            "  check:\n    needs: prepare\n"
            "  report:\n    needs: [prepare]\n    if: ${{ !cancelled() }}\n"
            "  strict:\n    needs: prepare\n    if: ${{ success() && true }}\n")
        self.assertTrue(job_gates(workflow, "check", "main-push"))
        self.assertFalse(job_gates(workflow, "check", "trusted-pr"))
        self.assertTrue(job_gates(workflow, "report", "trusted-pr"))
        self.assertFalse(job_gates(workflow, "strict", "trusted-pr"))
        self.assertEqual(skipped_main_jobs({"x.yml": dict(workflow, on={"push": None,
                                                                         "pull_request": None})},
                                           ["x.yml"])[0],
                         {("x.yml", "prepare"), ("x.yml", "check"), ("x.yml", "strict")})

    def test_replacements_compare_non_bazel_checks(self):
        main = ("    steps:\n      - name: Lint\n        run: python3 tools/lint.py\n"
                "      - name: Test\n        run: bazelisk test //...\n")
        workflow = parse_workflow(
            "jobs:\n  main:\n" + main + "  pr:\n    steps:\n"
            "      - name: Test\n        run: bazelisk test //...\n")
        self.assertEqual(invocation_differences(workflow, "main", "pr"),
                         {("Lint", "step-main-only", "")})
        changed = parse_workflow(
            "jobs:\n  main:\n" + main + "  pr:\n" + main.replace("lint.py", "lint.py --fast"))
        self.assertEqual(invocation_differences(changed, "main", "pr"),
                         {("Lint", "step-changed", "")})

    def test_shared_jobs_run_bazel_alike_on_main_and_pull_requests(self):
        observed = shared_job_differences(self.workflows, self.main_push_workflows)
        changed = {difference for difference in observed if difference[4] == "step-changed"}
        self._assert_matches_allowlist(
            observed - changed, SHARED_JOB_ALLOWLIST,
            "a shared job runs Bazel differently on pull requests")
        self._assert_matches_allowlist(
            {(name, job, step, scenario, digest)
             for name, job, scenario, step, _, digest in changed}, SHARED_STEP_ALLOWLIST,
            "a shared job runs a non-Bazel check differently on pull requests")

    def test_event_dependent_expressions_are_compared(self):
        workflow = parse_workflow(
            "on:\n  push:\n  pull_request:\n"
            "jobs:\n  build:\n    env:\n"
            "      SCOPE: ${{ github.event_name == 'push' && '//...' || '//donner/base/...' }}\n"
            "    steps:\n      - name: Test\n        run: |\n          bazelisk test $SCOPE\n"
            "      - name: Narrow\n        run: |\n"
            "          bazelisk test ${{ github.event_name == 'push' && '--keep_going' || '' }} //a\n")
        differences = shared_job_differences({"x.yml": workflow}, ["x.yml"])
        for scenario in PULL_REQUEST_SCENARIOS:
            self.assertIn(("x.yml", "build", scenario, "Test", "target-main-only", "//..."),
                          differences)
            self.assertIn(("x.yml", "build", scenario, "Narrow", "flag-main-only",
                           "--keep_going"), differences)
        self.assertEqual(render("${{ runner.temp }}/x ${{ github.event_name }}", "main-push"),
                         "${{ runner.temp }}/x push")

    def test_commented_out_bazel_commands_run_nothing(self):
        self.assertEqual(bazel_invocations("# bazelisk test //...\n", {}), [])
        self.assertEqual(bazel_invocations("echo start  # bazelisk test //...\n", {}), [])
        # A `#` inside quotes starts no comment, including after an escaped quote.
        self.assertEqual(len(bazel_invocations('echo "lane #1"; bazelisk test //...\n', {})), 1)
        self.assertEqual(
            len(bazel_invocations('echo "label \\" #1"; bazelisk test //...\n', {})), 1)
        self.assertEqual(failure_masks('echo "label \\" #1"; set +e'),
                         {'echo "label \\" #1"; set +e'})
        self.assertEqual(bazel_invocations("echo it\\'s  # bazelisk test //...\n", {}), [])
        # Bash ANSI-C quoting: a backslash escapes the quote inside $'...'.
        self.assertEqual(
            len(bazel_invocations("echo $'label \\' #1'; bazelisk test //...\n", {})), 1)
        self.assertEqual(failure_masks("echo $'label \\' #1'; set +e"),
                         {"echo $'label \\' #1'; set +e"})
        # An escaped blank continues the word, so the `#` after it starts no comment. An escaped
        # backslash before the blank leaves the blank unescaped.
        masked = "echo label\\ #1; bazelisk test //... || true"
        self.assertEqual(len(bazel_invocations(masked + "\n", {})), 1)
        self.assertEqual(failure_masks(masked), {masked})
        self.assertEqual(len(bazel_invocations("echo a\\\t#1; bazelisk test //...\n", {})), 1)
        self.assertEqual(len(bazel_invocations("echo a\\\\\\ #1; bazelisk test //...\n", {})), 1)
        self.assertEqual(bazel_invocations("echo a\\\\ # bazelisk test //...\n", {}), [])
        self.assertEqual(bazel_invocations("echo a\\ #1  # bazelisk test //...\n", {}), [])
        # An even run of backslashes before a newline is no continuation, so the next line is a
        # comment of its own.
        self.assertEqual(bazel_invocations("echo a\\\\\n# bazelisk test //... || true\n", {}), [])
        self.assertEqual(failure_masks("echo a\\\\\n# bazelisk test //... || true"), set())
        # A YAML plain scalar has no escapes, so its ` #` starts a YAML comment.
        steps = parse_workflow("steps:\n  - run: echo a\\ #1; bazelisk test //... || true\n")
        self.assertEqual(steps["steps"][0]["run"], "echo a\\")
        quoted = bazel_invocations('bazelisk test --test_arg="a #b" //a\n', {})
        self.assertEqual(quoted[0]["targets"], {"//a"})
        self.assertEqual(failure_masks('echo "lane #1"; set +e\nbazelisk test //...'),
                         {'echo "lane #1"; set +e'})
        workflow = parse_workflow(
            "jobs:\n  main:\n    steps:\n      - name: Test\n        run: bazelisk test //...\n"
            "  pr:\n    steps:\n      - name: Test\n        run: \"# bazelisk test //...\"\n")
        self.assertIn(("Test", "step-main-only", ""),
                      invocation_differences(workflow, "main", "pr"))

    def test_trigger_shorthand_and_activity_types(self):
        self.assertEqual(triggers("push"), {"push": None})
        self.assertEqual(triggers(["push", "pull_request"]), {"push": None, "pull_request": None})
        self.assertTrue(_push_runs_on_main("push"))
        self.assertTrue(_push_runs_on_main(["push", "pull_request"]))
        workflows = {
            "short.yml": parse_workflow("on: push\n"),
            "both.yml": parse_workflow("on: [push, pull_request]\n"),
            "closed.yml": parse_workflow(
                "on:\n  push:\n  pull_request:\n    types: [closed]\n"),
            "edited.yml": parse_workflow(
                "on:\n  push:\n  pull_request:\n"
                "    types: [opened, synchronize, reopened, edited]\n"),
        }
        self.assertEqual(trigger_differences(workflows, sorted(workflows)),
                         {("short.yml", "push-only"), ("closed.yml", "pull-request-types")})

    def test_shared_steps_that_stop_running_bazel_are_reported(self):
        workflow = parse_workflow(
            "on:\n  push:\n  pull_request:\n"
            "jobs:\n  build:\n    steps:\n      - name: Test\n        run: |\n"
            "          ${{ github.event_name == 'pull_request' && '#' || '' }} bazelisk test //...\n")
        self.assertEqual(shared_job_differences({"x.yml": workflow}, ["x.yml"]),
                         {("x.yml", "build", scenario, "Test", "step-main-only", "")
                          for scenario in PULL_REQUEST_SCENARIOS})

    def test_policy_event_inputs_are_the_only_inputs_ignored(self):
        step = ("      - name: Cache\n        uses: ./.github/actions/buildbuddy-cache\n"
                "        with:\n          trusted-pr: ${{ needs.gatekeeper.outputs.trusted_pr }}\n"
                "          download-outputs: %s\n")
        workflow = parse_workflow("on:\n  push:\n  pull_request:\njobs:\n  build:\n"
                                  "    steps:\n" + step % "minimal")
        self.assertEqual(shared_job_differences({"x.yml": workflow}, ["x.yml"]), set())
        changed = parse_workflow(
            "on:\n  push:\n  pull_request:\njobs:\n  build:\n    steps:\n"
            + step % "${{ github.event_name == 'push' && 'minimal' || 'all' }}")
        self.assertEqual({difference[:5] for difference in
                          shared_job_differences({"x.yml": changed}, ["x.yml"])},
                         {("x.yml", "build", scenario, "Cache", "step-changed")
                          for scenario in PULL_REQUEST_SCENARIOS})
        # The exemption belongs to the action that reads the input, not to the input name.
        other_action = parse_workflow(
            "on:\n  push:\n  pull_request:\njobs:\n  build:\n    steps:\n"
            "      - name: Scan\n        uses: ./.github/actions/security-scan\n"
            "        with:\n          trusted-pr: ${{ github.event_name == 'push' && 'strict' "
            "|| 'off' }}\n")
        self.assertEqual({difference[:5] for difference in
                          shared_job_differences({"x.yml": other_action}, ["x.yml"])},
                         {("x.yml", "build", scenario, "Scan", "step-changed")
                          for scenario in PULL_REQUEST_SCENARIOS})

    def test_non_bazel_checks_render_event_expressions(self):
        lint = ("      - name: Lint\n        run: python3 tools/lint.py "
                "${{ github.event_name == 'push' && '--strict' || '' }}\n")
        workflow = parse_workflow("on:\n  push:\n  pull_request:\n"
                                  "jobs:\n  build:\n    steps:\n" + lint)
        observed = shared_job_differences({"x.yml": workflow}, ["x.yml"])
        self.assertEqual({difference[:5] for difference in observed},
                         {("x.yml", "build", scenario, "Lint", "step-changed")
                          for scenario in PULL_REQUEST_SCENARIOS})
        # The digest identifies the rendered definitions: an edit to the step changes it.
        stricter = parse_workflow("on:\n  push:\n  pull_request:\n"
                                  "jobs:\n  build:\n    steps:\n"
                                  + lint.replace("--strict", "--strict --all"))
        self.assertFalse(observed & shared_job_differences({"x.yml": stricter}, ["x.yml"]))
        replaced = parse_workflow("jobs:\n  main:\n    steps:\n" + lint
                                  + "  pr:\n    steps:\n" + lint)
        self.assertEqual(invocation_differences(replaced, "main", "pr"),
                         {("Lint", "step-changed", "")})

    def test_branch_filters_follow_github_pattern_order(self):
        self.assertTrue(branch_filter_includes("main", ["**"]))
        self.assertFalse(branch_filter_includes("main", ["**", "!main"]))
        self.assertTrue(branch_filter_includes("main", ["!main", "**"]))
        self.assertTrue(branch_filter_includes("main", ["ma?n"]))
        self.assertFalse(branch_filter_includes("main", ["release/*"]))
        self.assertTrue(branch_filter_includes("main", ignore=["release/**"]))
        self.assertFalse(branch_filter_includes("main", ignore=["mai*"]))
        self.assertTrue(_push_runs_on_main({"push": {"branches-ignore": ["release/**"]}}))
        self.assertFalse(_push_runs_on_main({"push": {"tags": ["v*"]}}))
        workflows = {"x.yml": parse_workflow(
            "on:\n  push:\n    branches-ignore: [\"release/**\"]\n"
            "  pull_request:\n    branches: [\"**\", \"!main\"]\n")}
        self.assertEqual(trigger_differences(workflows, ["x.yml"]),
                         {("x.yml", "pull-request-branches")})

    def test_non_bazel_checks_compare_the_environment_they_read(self):
        main = ("    env:\n      MODE: strict\n      OTHER: a\n    steps:\n"
                "      - name: Lint\n        run: python3 tools/lint.py --mode \"$MODE\"\n")
        workflow = parse_workflow(
            "jobs:\n  main:\n" + main + "  pr:\n"
            + main.replace("MODE: strict", "MODE: fast").replace("OTHER: a", "OTHER: b"))
        self.assertEqual(invocation_differences(workflow, "main", "pr"),
                         {("Lint", "step-changed", "")})
        unread = parse_workflow("jobs:\n  main:\n" + main + "  pr:\n"
                                + main.replace("OTHER: a", "OTHER: b"))
        self.assertEqual(invocation_differences(unread, "main", "pr"), set())

    def test_main_only_cache_saves_are_not_checks(self):
        workflow = parse_workflow(
            "on:\n  push:\n  pull_request:\n"
            "jobs:\n  build:\n    steps:\n"
            "      - name: Save cache\n"
            "        if: github.event_name != 'pull_request' && github.ref == 'refs/heads/main'\n"
            "        uses: actions/cache/save@v6\n"
            "      - name: Publish\n"
            "        if: github.event_name != 'pull_request'\n"
            "        run: echo publish\n")
        self.assertEqual(skipped_main_steps({"x.yml": workflow}, ["x.yml"]), {
            ("x.yml", "build", "Publish", "trusted-pr"),
            ("x.yml", "build", "Publish", "untrusted-pr"),
            ("x.yml", "build", "Publish", "renovate-pr"),
        })

    def test_hosted_subsets_run_main_steps_unchanged(self):
        observed = set()
        for (name, job_name, scenario), (subset, covered) in HOSTED_SUBSETS.items():
            workflow = self.workflows[name]
            observed |= {(name, job_name, subset) + difference
                         for difference in subset_differences(workflow, job_name, subset,
                                                              covered)}
            with self.subTest(subset=subset):
                self.assertEqual(subset_setup_differences(workflow, job_name, subset, covered),
                                 [])
                # Every variable the subset defines has main's value.
                main_environment = _environment(workflow, _jobs(workflow)[job_name])
                for key, value in _environment(workflow, _jobs(workflow)[subset]).items():
                    self.assertEqual(main_environment.get(key), value, key)
        self._assert_matches_allowlist(observed, SUBSET_ALLOWLIST,
                                       "a hosted subset runs a main step differently")

    def test_subset_order_problems_are_found(self):
        main = parse_workflow(
            "steps:\n      - name: Setup\n        run: x\n      - name: Profile\n"
            "        run: y\n      - name: Test\n        run: z\n")
        reordered = parse_workflow(
            "steps:\n      - name: Setup\n        run: x\n      - name: Test\n"
            "        run: z\n      - name: Profile\n        run: y\n"
            "      - name: Write bazelrc\n        run: echo test >> .bazelrc\n")
        self.assertEqual(subset_order_problems(main, reordered), [
            ("Profile", "out of order"),
            ("Write bazelrc", "not in main's job"),
        ])
        self.assertEqual(subset_order_problems(main, main), [])

    def test_hosted_subsets_run_only_for_affected_trusted_pull_requests(self):
        for (name, _, scenario), (subset, _) in HOSTED_SUBSETS.items():
            workflow = self.workflows[name]
            with self.subTest(subset=subset):
                # job_gates includes the jobs the subset needs.
                self.assertTrue(job_gates(workflow, subset, scenario))
                self.assertFalse(job_gates(workflow, subset, "trusted-pr-native-only"))
                # Main pushes, untrusted and Renovate PRs already run the whole hosted job.
                for other in ("main-push", "untrusted-pr", "renovate-pr"):
                    self.assertFalse(job_gates(workflow, subset, other), other)
                # An infrastructure change falls back to //..., which reaches every browser test.
                self.assertTrue(job_gates(workflow, subset, "trusted-pr-fallback"))

    def test_every_allowlist_entry_has_a_reason(self):
        for name, (_, reason) in STEP_OUTPUTS.items():
            self.assertGreater(len(reason.strip()), 20, name)
        for allowlist in (TRIGGER_ALLOWLIST, MAIN_ONLY_JOBS, INVOCATION_ALLOWLIST,
                          STEP_ALLOWLIST, COUNTERPART_ALLOWLIST, FAILURE_SWALLOW_ALLOWLIST,
                          SHARED_JOB_ALLOWLIST, SHARED_STEP_ALLOWLIST, SUBSET_ALLOWLIST,
                          SUBSET_EXTRA_STEPS):
            for key, reason in allowlist.items():
                self.assertGreater(len(reason.strip()), 20, key)

    def test_invocation_comparison_reports_each_kind_of_difference(self):
        main = textwrap_job(
            'X_FLAGS: "--test_env=A=1"',
            "tools/ci_bazel_test.sh bazelisk test --config=ci --test_tag_filters=-manual,-perf "
            "$X_FLAGS //...",
        )
        replacement = textwrap_job(
            'X_FLAGS: "--test_env=B=1 --local_test_jobs=3"',
            "bazelisk --nohome_rc test --config=ci --test_tag_filters=-manual $X_FLAGS //...",
        )
        workflow = parse_workflow("jobs:\n  main:\n" + main + "  pr:\n" + replacement)
        self.assertEqual(invocation_differences(workflow, "main", "pr"), {
            ("Test", "exit-status-wrapper", ""),
            ("*", "startup-pr-only", "--nohome_rc"),
            ("*", "flag-pr-only", "--local_test_jobs=3"),
            ("Test", "test-env-main-only", "A=1"),
            ("Test", "test-env-pr-only", "B=1"),
            ("Test", "tag-filter-main-only", "-perf"),
        })


def textwrap_job(environment, command):
    return ("    env:\n"
            "      %s\n"
            "    steps:\n"
            "      - name: Test\n"
            "        run: |\n"
            "          %s\n") % (environment, command)


if __name__ == "__main__":
    unittest.main()
