"""Pins review-driven security workflow selection and scheduling policy."""

from collections import Counter
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import textwrap
import unittest

from python.runfiles import runfiles
from tools import test_timing


BAZEL_DIFF_TAG = "v18.1.0"
BAZEL_DIFF_SHA256 = "eadec6d0ca0991de78bd9e063bfed993a359ed3bc60e507818077c300b6f58e0"
CLOC_SHA256 = "73570f9da159fab13846038de7c3d8772554117c04117281dcbe6e5c7b988264"
DOXYGEN_SHA256 = "0ec2e5b2c3cd82b7106d19cb42d8466450730b8cb7a9e85af712be38bf4523a1"

# The repository's default workflow token can write, so every workflow states its token scopes.
# Only these jobs may request a write scope, and each must request exactly the listed ones.
WRITE_SCOPES = {
    (".github/workflows/bcr_preflight.yml", "attest-artifacts"): {"attestations", "id-token"},
    (".github/workflows/codeql.yml", "analyze"): {"security-events"},
    (".github/workflows/deploy_docs.yaml", "deploy"): {"id-token", "pages"},
    (".github/workflows/release.yml", "publish-release-artifacts"): {"contents"},
    (".github/workflows/release_candidate.yml", "attest-record"): {"attestations", "id-token"},
    (".github/workflows/rerun_runner_loss.yml", "rerun"): {"actions"},
}

# Exact reviewed step consumers: workflow, job, name, id, action (or the run script marker).
SECRET_USES = {
    (".github/workflows/badges.yaml", "update-badges", "Create Lines-of-Code-Badge", None,
     "schneegans/dynamic-badges-action@v1.9.0"): {"GIST_SECRET"},
    (".github/workflows/badges.yaml", "update-badges", "Create Test-Lines-of-Code-Badge", None,
     "schneegans/dynamic-badges-action@v1.9.0"): {"GIST_SECRET"},
    (".github/workflows/badges.yaml", "update-badges", "Create Comments-Badge", None,
     "schneegans/dynamic-badges-action@v1.9.0"): {"GIST_SECRET"},
    (".github/workflows/coverage.yml", "upload-coverage", "Upload coverage to Codecov", None,
     "codecov/codecov-action@v7"): {"CODECOV_TOKEN"},
    (".github/workflows/editor_wasm.yml", "build", "Configure BuildBuddy remote cache", None,
     "./.github/actions/buildbuddy-cache"): {"BUILDBUDDY_API_KEY"},
    (".github/workflows/editor_wasm.yml", "build", "Configure BuildBuddy read-only cache", None,
     "./.github/actions/buildbuddy-cache"): {"BUILDBUDDY_API_KEY_READONLY"},
    (".github/workflows/github_hosted_nightly.yml", "build", "Configure BuildBuddy remote cache", None,
     "./.github/actions/buildbuddy-cache"): {"BUILDBUDDY_API_KEY"},
    (".github/workflows/main.yml", "linux", "Configure BuildBuddy remote cache", "buildbuddy",
     "./.github/actions/buildbuddy-cache"): {"BUILDBUDDY_API_KEY"},
    (".github/workflows/main.yml", "linux", "Configure BuildBuddy read-only cache", None,
     "./.github/actions/buildbuddy-cache"): {"BUILDBUDDY_API_KEY_READONLY"},
    (".github/workflows/main.yml", "macos", "Configure BuildBuddy remote cache", None,
     "./.github/actions/buildbuddy-cache"): {"BUILDBUDDY_API_KEY"},
    (".github/workflows/main.yml", "macos", "Configure BuildBuddy read-only cache", None,
     "./.github/actions/buildbuddy-cache"): {"BUILDBUDDY_API_KEY_READONLY"},
    (".github/workflows/main.yml", "macos-browser", "Configure BuildBuddy remote cache", None,
     "./.github/actions/buildbuddy-cache"): {"BUILDBUDDY_API_KEY"},
    (".github/workflows/main.yml", "macos-browser", "Configure BuildBuddy read-only cache", None,
     "./.github/actions/buildbuddy-cache"): {"BUILDBUDDY_API_KEY_READONLY"},
    (".github/workflows/main.yml", "macos-self-hosted", "Configure BuildBuddy remote cache", None,
     "./.github/actions/buildbuddy-cache"): {"BUILDBUDDY_API_KEY"},
    (".github/workflows/main.yml", "macos-hosted-parity", "Configure BuildBuddy remote cache",
     None, "./.github/actions/buildbuddy-cache"): {"BUILDBUDDY_API_KEY"},
    (".github/workflows/publish_bcr.yml", "publish-fork", "Create the fork branch only if absent", None,
     "<run>"): {"PUBLISH_TOKEN"},
    (".github/workflows/sanitizers-pr.yml", "asan-geode", "Configure BuildBuddy remote cache",
     None, "./.github/actions/buildbuddy-cache"): {"BUILDBUDDY_API_KEY"},
    (".github/workflows/sanitizers-pr.yml", "asan-geode", "Configure BuildBuddy read-only cache",
     None, "./.github/actions/buildbuddy-cache"): {"BUILDBUDDY_API_KEY_READONLY"},
    (".github/workflows/sanitizers.yml", "asan", "Configure BuildBuddy remote cache", None,
     "./.github/actions/buildbuddy-cache"): {"BUILDBUDDY_API_KEY"},
    (".github/workflows/sanitizers.yml", "ubsan", "Configure BuildBuddy remote cache", None,
     "./.github/actions/buildbuddy-cache"): {"BUILDBUDDY_API_KEY"},
}

# The only jobs that call a reusable workflow: (caller, job) to (callee, forwarded secrets). A
# called workflow receives only the secrets its caller passes, so a caller forwards each listed
# secret under its own name and nothing else. The callee declares exactly these names and reads
# each one only in its reviewed steps above. pr_base_change.yml runs main.yml's full CI when a
# pull request's base branch changes; it forwards the repository secrets main.yml's steps read.
REVIEWED_WORKFLOW_CALLS = {
    (".github/workflows/pr_base_change.yml", "base-change-ci"): (
        "./.github/workflows/main.yml", frozenset({"BUILDBUDDY_API_KEY_READONLY"})),
}

# The only jobs that may run on self-hosted runners, with the gatekeeper output admitting each.
SELF_HOSTED_JOBS = {
    (".github/workflows/main.yml", "linux-self-hosted"): "use_self_hosted_linux",
    (".github/workflows/main.yml", "macos-self-hosted"): "use_self_hosted_macos",
    (".github/workflows/main.yml", "macos-hosted-parity"): "use_self_hosted_macos",
    (".github/workflows/main.yml", "coverage-contract"): "use_self_hosted_linux",
}
# Reviewed status functions a self-hosted job puts before its trusted gate, so it runs after a
# needed job failed or was skipped. Joined with `&&`, it still requires the whole gate.
SELF_HOSTED_STATUS_PREFIX = {
    (".github/workflows/main.yml", "coverage-contract"): "!cancelled() && ",
}
# Reviewed conditions a self-hosted job adds after its trusted gate. Each may only narrow it: it
# follows the gate with `&&`, so it can skip a trusted run but never admit another one.
SELF_HOSTED_NARROWING = {
    (".github/workflows/main.yml", "macos-hosted-parity"):
        " && (needs.determine-targets.outputs.fallback == 'true' || "
        "needs.determine-targets.outputs.hosted_parity_affected == 'true')",
}

# A runner label set is GitHub-hosted only when it is exactly one hosted image label. Any other
# label, label list or runner group can route a job to a self-hosted runner.
HOSTED_IMAGES = frozenset({"ubuntu-24.04", "ubuntu-latest", "xcode-27", "macos-latest"})

SELF_HOSTED_GATE_PREFIX = (
    "${{ needs.gatekeeper.outputs.is_duplicate_push != 'true' && "
    "(github.ref == 'refs/heads/main' || github.event_name == 'pull_request') && "
    "(github.event_name != 'pull_request' || needs.gatekeeper.outputs.docs_only != 'true') && ")

# Reviewed `runs-on` expressions that are neither a literal label nor `${{ matrix.<key> }}`, with
# every label each can produce.
REVIEWED_RUNNER_EXPRESSIONS = {
    (".github/workflows/codeql.yml", "analyze"): (
        "${{ (matrix.language == 'swift' && 'macos-latest') || 'ubuntu-latest' }}",
        {"macos-latest", "ubuntu-latest"}),
}

# Local actions that upload an artifact themselves, and why each one's contents are reviewed.
REVIEWED_UPLOAD_ACTIONS = {
    # Copies only test.log, test.xml and test.outputs, without following symlinks; pinned by
    # test_artifact_collection_omits_nested_symlinks.
    "./.github/actions/upload-bazel-test-artifacts",
}

# Steps that save an Actions cache. Self-hosted jobs never save one: their outputs would reach
# hosted runs that restore the cache.
CACHE_SAVE = re.compile(r"uses: (?:actions/cache@|actions/cache/save@|"
                        r"bazel-contrib/setup-bazel@|\./\.github/actions/cache-bazel-actions)")

# Only pushes to main write the shared Actions cache. Pull requests and branch pushes restore
# main's entries (GitHub lets every branch read the default branch's caches) but never save:
# a PR-scoped entry is unreadable by other PRs and evicts main's entries from the repository
# budget. Scheduled and manual runs on main may save under the same condition.
MAIN_CACHE_WRITE = "github.event_name != 'pull_request' && github.ref == 'refs/heads/main'"
# A pull_request run's ref is its merge ref, so a bare main-ref test is equally main-only for
# the workflows' triggers (privileged pull_request_target is banned separately).
MAIN_CACHE_WRITE_FORMS = (MAIN_CACHE_WRITE, "github.ref == 'refs/heads/main'")
# External actions whose cache behavior is reviewed: none of them saves an Actions cache except
# the ones checked below. A new external action must be reviewed and added here, since an
# action can cache under any name; local composite actions are scanned step by step.
REVIEWED_EXTERNAL_ACTIONS = {
    "actions/attest-build-provenance", "actions/cache", "actions/cache/restore",
    "actions/cache/save", "actions/checkout", "actions/configure-pages",
    "actions/download-artifact", "actions/github-script", "actions/setup-java",
    "actions/setup-node", "actions/upload-artifact", "actions/upload-pages-artifact",
    "bazel-contrib/publish-to-bcr", "bazel-contrib/setup-bazel", "codecov/codecov-action",
    "github/codeql-action/analyze", "github/codeql-action/init", "nick-fields/retry",
    "schneegans/dynamic-badges-action",
}
# Setup actions with a built-in cache that saves on every event, whatever the trigger.
SELF_SAVING_SETUP = {"actions/setup-node", "actions/setup-python", "actions/setup-java",
                     "actions/setup-go", "actions/setup-dotnet"}


def _main_write_only(condition, expression_required=False):
    """Whether a condition can be true only for a main push, schedule or dispatch.

    An `if:` may be a bare expression; an input such as `cache-save` is a literal string
    unless it is one `${{ }}` expression. Text outside a single expression makes GitHub treat
    the whole value as a non-empty, always-true string, so any leftover delimiter fails.
    """
    condition = condition.strip()
    wrapped = condition.startswith("${{") and condition.endswith("}}")
    if wrapped:
        condition = condition[3:-2].strip()
    if (expression_required and not wrapped) or "${{" in condition or "}}" in condition:
        return False
    for form in MAIN_CACHE_WRITE_FORMS:
        rest = condition[len(form):] if condition.startswith(form) else None
        # Further conjuncts may only narrow the main-only condition.
        if rest == "" or (rest and rest.startswith(" && ") and "||" not in rest
                          and "(" not in rest):
            return True
    return False


def _step_key(step, key, indent):
    """One key of a step at its own indentation (the step's first line included)."""
    first, _, rest = step.partition("\n")
    pattern = r"%s:[ \t]*(.*)$" % re.escape(key)
    match = re.match(pattern, first.strip())
    if not match:
        match = re.search(r"(?m)^ {%d}%s" % (indent, pattern), rest)
    return match.group(1).split(" #", 1)[0].strip() if match else None


def _with_key(step, key, indent):
    """One `with:` input of a step, read only from its `with:` block."""
    lines = step.splitlines()
    for index, line in enumerate(lines):
        if re.match(r"^ {%d}with:\s*$" % indent, line):
            block = "\n".join(_indented_block(lines[index + 1:], indent))
            match = re.search(r"(?m)^ {%d}%s:[ \t]*(.*)$" % (indent + 2, re.escape(key)), block)
            return match.group(1).split(" #", 1)[0].strip() if match else None
    return None


def _step_action(step, indent):
    """The action a step uses, unquoted and lower-cased without its version, or None."""
    value = _step_key(step, "uses", indent)
    return value.strip("'\"").split("@", 1)[0].lower() if value else None


def _setup_action_saves(action, step, indent):
    """Whether a setup action step enables a built-in cache, which saves on every event."""
    cache = _with_key(step, "cache", indent)
    automatic = _with_key(step, "package-manager-cache", indent)
    if action == "actions/setup-node":
        return cache is not None or automatic != "false"
    if action == "actions/setup-go":
        return cache != "false"
    return cache is not None


def _cache_step_problem(step, indent):
    if not _main_write_only(_step_key(step, "if", indent) or ""):
        return "saves an Actions cache outside main"
    return None


def _setup_bazel_problem(step, indent):
    save = _with_key(step, "cache-save", indent) or ""
    if save != "false" and not _main_write_only(save, expression_required=True):
        return "lets setup-bazel save outside main"
    return None


def _cache_write_problem(step, indent):
    """Why one step could save an Actions cache outside main, or None."""
    if step[:1] in "{\"'*&":
        return "is written in a step form the cache policy cannot read"
    action = _step_action(step, indent) or ""
    checks = {"actions/cache": _cache_step_problem, "actions/cache/save": _cache_step_problem,
              "bazel-contrib/setup-bazel": _setup_bazel_problem}
    if action in checks:
        return checks[action](step, indent)
    if action in SELF_SAVING_SETUP and _setup_action_saves(action, step, indent):
        return "uses a setup action cache that saves on every event"
    if action and not action.startswith("./") and action not in REVIEWED_EXTERNAL_ACTIONS:
        return "uses an action with unreviewed cache behavior: %s" % action
    return None


def _cache_write_violations(files):
    """Every step that could save an Actions cache outside a main push, schedule or dispatch."""
    steps = []
    for path, text in sorted(files.items()):
        if path.startswith(".github/workflows/"):
            steps.extend(("%s:%s" % (path, name), step, 8)
                         for name, job in _jobs(text).items() for step in _steps(job))
        else:
            steps.extend(("%s:<action>" % path, step, 6) for step in _steps(text, indent=4))
    return ["%s %s" % (where, problem) for where, problem in
            ((where, _cache_write_problem(step, indent)) for where, step, indent in steps)
            if problem]


BUILDBUDDY_ACTION = "./.github/actions/buildbuddy-cache"
PR_TRUST_ACTION = "./.github/actions/pr-trust"
# Every Bazel lane on the BuildBuddy remote cache. The release-candidate CLI builds in
# bcr_preflight.yml stay off it: their attested bytes must not depend on a third-party cache.
BUILDBUDDY_JOBS = {
    (".github/workflows/editor_wasm.yml", "build"),
    (".github/workflows/github_hosted_nightly.yml", "build"),
    (".github/workflows/main.yml", "linux"),
    (".github/workflows/main.yml", "macos"),
    # Hosted macOS browser subset for trusted maintainer PRs, wired exactly like `macos`.
    (".github/workflows/main.yml", "macos-browser"),
    (".github/workflows/main.yml", "macos-self-hosted"),
    (".github/workflows/main.yml", "macos-hosted-parity"),
    (".github/workflows/sanitizers-pr.yml", "asan-geode"),
    (".github/workflows/sanitizers.yml", "asan"),
    (".github/workflows/sanitizers.yml", "ubsan"),
}
# The self-hosted lanes on BuildBuddy: the macOS fast lane and its hosted-parity subset. They run
# only for trusted maintainer pull requests, so each holds the read-write key and no Renovate
# reader, and its last step removes the key from the persistent runner whatever the job's outcome.
SELF_HOSTED_BUILDBUDDY_JOBS = {(".github/workflows/main.yml", "macos-self-hosted"),
                               (".github/workflows/main.yml", "macos-hosted-parity")}
BUILDBUDDY_CLEANUP = ("      - name: Remove BuildBuddy credentials\n"
                      "        if: always()\n"
                      "        shell: bash\n"
                      "        run: rm -rf \"${RUNNER_TEMP}/buildbuddy\" "
                      "\"${GITHUB_WORKSPACE}/.buildbuddy.bazelrc\"\n")
# Jobs that may classify pull request trust with the shared rule (.github/actions/pr-trust) for
# the jobs that need them.
TRUST_JOBS = ("gatekeeper", "trust")
WRITE_ENVIRONMENT = "buildbuddy-write"
BUILDBUDDY_DOWNLOADS = {"minimal", "toplevel", "all"}
# The probe in the action sees the cache only at setup. When a later Bazel command cannot reach
# it, Bazel ends the build (exit code 34) unless both of these let the action run locally.
BUILDBUDDY_FALLBACK = ("build --remote_local_fallback",
                       "build --incompatible_remote_local_fallback_for_remote_cache")
REMOTE_FALLBACK_FLAG = re.compile(r"--(?:no)?(?:incompatible_)?remote_local_fallback")
# A path-filtered workflow with a BuildBuddy lane must run when the action changes.
BUILDBUDDY_ACTION_FILES = ".github/actions/buildbuddy-cache/**"
SECRET_NAMES_BUILDBUDDY = ("BUILDBUDDY_API_KEY", "BUILDBUDDY_API_KEY_READONLY")
# Anything that can start Bazel: the client itself, directly or by path, and the repository's
# wrappers that run it.
BAZEL_COMMAND = re.compile(r"(?:^|[\s/\"'(=])bazel(?:isk)?\b(?!-)|\$\{?bazel_bin\b|"
                           r"ci_bazel_test\.sh|run-browser-ci\.sh|test_with_exclusions\.py")


def _exports_trust(workflow, name):
    job = _jobs(workflow).get(name, "")
    return (any(_step_identity_value(step, "id") == "pr_trust" and
                _step_action(step, 8) == PR_TRUST_ACTION for step in _steps(job)) and
            all("      %s_pr: ${{ steps.pr_trust.outputs.%s_pr }}" % (kind, kind) in job
                for kind in ("trusted", "renovate")))


def _trust_gates(workflow, job):
    """The trust gates this job may read: only outputs of a needed job that runs pr-trust."""
    needs = _labels(_job_key(job, "needs").strip("[]"))
    return {"needs.%s.outputs" % name for name in TRUST_JOBS
            if name in needs and _exports_trust(workflow, name)}


def _write_conditions(gates):
    """Every reviewed condition under which a job may hold the read-write key."""
    return {MAIN_CACHE_WRITE} | {"(%s) || %s.trusted_pr == 'true'" % (MAIN_CACHE_WRITE, gate)
                                 for gate in gates}


def _job_environment(job):
    """A job's `environment:` as (name, deployment), the inline name alone, or None."""
    lines = job.splitlines()
    for index, line in enumerate(lines):
        if line.startswith("    environment:"):
            inline = line.split(":", 1)[1].split(" #", 1)[0].strip()
            if inline:
                return inline, None
            block = _mapping(_indented_block(lines[index + 1:], 4))
            return block.get("name"), block.get("deployment")
    return None


def _write_environment_condition(job):
    """The condition under which this job binds the write environment, or None."""
    environment = _job_environment(job)
    if not environment or environment[1] != "false":
        return None
    match = re.fullmatch(r"\$\{\{ \((.*)\) && '%s' \|\| '' \}\}" % WRITE_ENVIRONMENT,
                         environment[0] or "")
    return match.group(1) if match else None


def _evaluate_condition(condition, event, ref, gates_values):
    """Evaluate one reviewed write condition for a run, without trusting arbitrary text."""
    atoms = {"github.event_name": event, "github.ref": ref}
    for gate, values in gates_values.items():
        atoms.update({"%s.%s" % (gate, key): value for key, value in values.items()})
    tokens = re.findall(r"\(|\)|&&|\|\||==|!=|'[^']*'|[A-Za-z_][A-Za-z0-9_.-]*", condition)
    if "".join(tokens) != condition.replace(" ", ""):
        raise ValueError("unreviewable condition: %s" % condition)
    python = []
    for token in tokens:
        if token in ("&&", "||"):
            python.append(" and " if token == "&&" else " or ")
        elif token in ("(", ")", "==", "!=") or token.startswith("'"):
            python.append(token)
        else:
            python.append(repr(atoms.get(token, "")))
    return eval("".join(python), {"__builtins__": {}})  # Tokens are restricted above.


def _writer_step_problem(condition, inputs, gates):
    if inputs["api-key"] != "${{ secrets.BUILDBUDDY_API_KEY }}":
        return "passes the read-write key through an unreviewed input"
    if condition == MAIN_CACHE_WRITE:
        return None
    for gate in gates:
        trusted = "(%s) || %s.trusted_pr == 'true'" % (MAIN_CACHE_WRITE, gate)
        if (condition == trusted and inputs["trusted-pr"] == "${{ %s.trusted_pr }}" % gate
                and inputs["renovate-pr"] == "${{ %s.renovate_pr }}" % gate):
            return None
    return "gives the read-write key to a run that is neither main nor a trusted pull request"


# Every job that binds an environment, and the literal name it binds. The BuildBuddy jobs bind
# the write environment through the reviewed conditional form instead.
REVIEWED_ENVIRONMENTS = {
    (".github/workflows/deploy_docs.yaml", "deploy"): "github-pages",
    (".github/workflows/publish_bcr.yml", "publish-fork"): "bcr-fork-preparation",
}


def _write_environment_violations(path, name, workflow, job):
    """Only reviewed jobs bind an environment. A job holding the read-write key binds the write
    environment exactly when its writer step may run; no other job binds it in any spelling."""
    where = "%s:%s" % (path, name)
    environment = _job_environment(job)
    writes = "BUILDBUDDY_API_KEY" in _secret_reads(job)
    if not writes:
        if environment is None:
            return []
        literal = REVIEWED_ENVIRONMENTS.get((path, name))
        bound = (environment[0] or "").strip().strip("'\"")
        # Environment names are case-insensitive, and an expression can name any environment.
        if literal is None or bound.lower() != literal.lower() or "${{" in bound:
            return ["%s binds an unreviewed environment %s" % (where, environment[0])]
        return []
    condition = _write_environment_condition(job)
    if condition not in _write_conditions(_trust_gates(workflow, job)):
        return ["%s binds the write environment outside main and trusted pull requests" % where]
    writer = [_step_key(step, "if", 8) for step in _steps(job)
              if _secret_reads(step) == {"BUILDBUDDY_API_KEY"}]
    if writer != [condition]:
        return ["%s writer step condition differs from its environment condition" % where]
    return []


def _reader_step_problem(condition, inputs, gates):
    if inputs["read-only-api-key"] != "${{ secrets.BUILDBUDDY_API_KEY_READONLY }}":
        return "passes the read-only key through an unreviewed input"
    for gate in gates:
        if (condition == "%s.renovate_pr == 'true'" % gate and
                inputs["renovate-pr"] == "${{ %s.renovate_pr }}" % gate):
            return None
    return "gives the read-only key to a run that is not a Renovate pull request"


BUILDBUDDY_STEP_CHECKS = {
    frozenset({"BUILDBUDDY_API_KEY"}): _writer_step_problem,
    frozenset({"BUILDBUDDY_API_KEY_READONLY"}): _reader_step_problem,
}


def _buildbuddy_step_problem(workflow, job, step):
    """Why one BuildBuddy step could hand a key to the wrong run, or None."""
    inputs = {key: _with_key(step, key, 8) for key in
              ("api-key", "read-only-api-key", "trusted-pr", "renovate-pr", "download-outputs")}
    if inputs["download-outputs"] not in BUILDBUDDY_DOWNLOADS:
        return "does not choose --remote_download_outputs"
    check = BUILDBUDDY_STEP_CHECKS.get(frozenset(_secret_reads(step)))
    if check is None:
        return "configures BuildBuddy without exactly one reviewed key"
    return check(_step_key(step, "if", 8) or "", inputs, _trust_gates(workflow, job))


def _buildbuddy_step_violations(workflow, where, job):
    """Problems of each step that configures BuildBuddy or reads one of its keys."""
    violations = []
    steps = _steps(job)
    for step in steps:
        uses_action = _step_action(step, 8) == BUILDBUDDY_ACTION
        if not uses_action and _secret_reads(step) & set(SECRET_NAMES_BUILDBUDDY):
            violations.append("%s reads a BuildBuddy key outside the action" % where)
        problem = uses_action and _buildbuddy_step_problem(workflow, job, step)
        if problem:
            violations.append("%s %s" % (where, problem))
    return violations


def _buildbuddy_job_violations(path, name, workflow, steps):
    """Job-level problems of a lane that configures BuildBuddy."""
    where = "%s:%s" % (path, name)
    configured = [index for index, step in enumerate(steps)
                  if _step_action(step, 8) == BUILDBUDDY_ACTION]
    violations = []
    self_hosted = _runs_self_hosted(path, name, _jobs(workflow)[name])
    if self_hosted and (path, name) not in SELF_HOSTED_BUILDBUDDY_JOBS:
        violations.append("%s uses BuildBuddy on a self-hosted runner" % where)
    if self_hosted and not (_jobs(workflow)[name].rstrip("\n") + "\n").endswith(
            "\n" + BUILDBUDDY_CLEANUP):
        violations.append("%s does not remove the BuildBuddy key in its last step" % where)
    if any(BAZEL_COMMAND.search(step) for step in steps[:max(configured)]):
        violations.append("%s runs Bazel before configuring BuildBuddy" % where)
    if REMOTE_FALLBACK_FLAG.search(workflow.split("\njobs:\n", 1)[0] + _jobs(workflow)[name]):
        violations.append("%s overrides the BuildBuddy local fallback" % where)
    kinds = Counter(frozenset(_secret_reads(steps[index])) for index in configured)
    pull_requests = "pull_request" in _triggers(workflow) and not self_hosted
    expected = Counter({frozenset({"BUILDBUDDY_API_KEY"}): 1})
    if pull_requests:
        expected[frozenset({"BUILDBUDDY_API_KEY_READONLY"})] = 1
    if kinds != expected:
        violations.append("%s does not pair one writer with %s" % (
            where, "one Renovate reader" if pull_requests else "no reader"))
    return violations


def _buildbuddy_violations(files):
    """Every way the workflows could widen BuildBuddy access or miss an in-scope hosted lane."""
    violations = []
    seen = set()
    for path, workflow in sorted(_workflows(files).items()):
        for name, job in _jobs(workflow).items():
            steps = _steps(job)
            violations.extend(_buildbuddy_step_violations(workflow, "%s:%s" % (path, name), job))
            violations.extend(_write_environment_violations(path, name, workflow, job))
            if BUILDBUDDY_ACTION in [_step_action(step, 8) for step in steps]:
                seen.add((path, name))
                violations.extend(_buildbuddy_job_violations(path, name, workflow, steps))
    if seen != BUILDBUDDY_JOBS:
        violations.append("BuildBuddy lanes changed: %s" % sorted(seen))
    return violations + _buildbuddy_path_filter_violations(files, seen)


def _fallback_lines(fragment):
    """A generated fragment's lines that name any local fallback flag, sorted."""
    return sorted(line for line in fragment.splitlines() if "fallback" in line)


def _buildbuddy_path_filter_violations(files, lanes):
    """Path filters that would let the BuildBuddy action change without running its lanes."""
    return ["%s has a BuildBuddy lane, but its %s paths skip the action" % (path, trigger)
            for path in sorted({path for path, _ in lanes})
            for trigger, entries in sorted(_trigger_paths(files[path]).items())
            if BUILDBUDDY_ACTION_FILES not in entries]


# Files a self-hosted job may upload directly. Raw Bazel event streams and execution logs record
# the runner's effective configuration, so they never leave the runner.
SELF_HOSTED_UPLOAD_FILES = {"coverage-receipt.json", "manifest.txt", "profile.gz", "report.md",
                            "stall.txt", "target-list.txt"}

# Triggers that run with repository credentials on behalf of an event someone else controls.
PRIVILEGED_TRIGGERS = {"discussion_comment", "issue_comment", "issues", "pull_request_review",
                       "pull_request_review_comment", "pull_request_target", "workflow_run"}

# Event fields that a script may interpolate directly: commit IDs and numbers only.
SCRIPT_SAFE_EVENT_FIELDS = {"github.event.pull_request.base.sha",
                            "github.event.pull_request.head.sha",
                            "github.event.pull_request.number"}

UNTRUSTED_CONTEXT = re.compile(r"github\.event\b(?:\.[A-Za-z0-9_.*-]+)?|github\.head_ref\b|"
                              r"\binputs\b(?:\.[A-Za-z0-9_.*-]+)?")
UPLOADED_EVENT_LOG = re.compile(r"(?i)(?:^|/)[^/]*bep[^/]*\.json$|exec(?:ution)?[._]log")
WORKSPACE_ROOTS = {".", "./", "${{ github.workspace }}", "${{ github.workspace }}/"}
# Private-network addresses, private DNS names and personal home directories have no place in
# public CI definitions.
PRIVATE_NETWORK_LITERAL = re.compile(
    r"\b(?:10\.\d{1,3}|192\.168|172\.(?:1[6-9]|2\d|3[01])|169\.254|"
    r"100\.(?:6[4-9]|[7-9]\d|1[01]\d|12[0-7]))(?:\.\d{1,3}){2}\b|"
    r"(?i:\bf[cd][0-9a-f]{2}:[0-9a-f]{0,4}:[0-9a-f:]*)|"
    r"(?i:\b[a-z0-9-]+(?:\.[a-z0-9-]+)*\.(?:local|lan|internal|home\.arpa|ts\.net)\b)|"
    r"/(?:Users|home)/(?!(?:runner|linuxbrew)\b)[A-Za-z0-9._-]+")


# The one reviewed workflow_run consumer: it reruns a main run's failed jobs once when every failure
# is a hosted-runner loss. A privileged trigger runs with the default branch's definition and an
# elevated token, so its shape is pinned: it checks out only the default branch's classifier, runs
# exactly the reviewed commands, reads no head ref of the triggering run, and acts only on main
# push first attempts.
RUNNER_LOSS_WORKFLOW = ".github/workflows/rerun_runner_loss.yml"
RUNNER_LOSS_TRIGGERS = {"workflow_run", "workflow_dispatch"}
RUNNER_LOSS_SCRIPT = "tools/ci_runner_loss_rerun.py"
RUNNER_LOSS_COMMANDS = {
    "rerun": 'python3 %s --mode rerun --run-id "$RUN_ID"' % RUNNER_LOSS_SCRIPT,
    "dry-run": 'python3 %s --mode dry-run --run-id "$RUN_ID"' % RUNNER_LOSS_SCRIPT,
}
# The only environment each job's step receives: the workflow token and the run to classify.
RUNNER_LOSS_STEP_ENV = {
    "rerun": {"GH_TOKEN": "${{ github.token }}",
              "RUN_ID": "${{ github.event.workflow_run.id }}"},
    "dry-run": {"GH_TOKEN": "${{ github.token }}", "RUN_ID": "${{ inputs.run_id }}"},
}
RUNNER_LOSS_WORKFLOW_ENV = ['  DO_NOT_TRACK: "1"', '  ASPECT_TOOLS_TELEMETRY: "-all"']
RUNNER_LOSS_JOB_GATES = {
    "rerun": ("github.event_name == 'workflow_run'",
              "github.event.workflow_run.head_branch == 'main'",
              "github.event.workflow_run.event == 'push'",
              "github.event.workflow_run.conclusion == 'failure'",
              "github.event.workflow_run.run_attempt == 1"),
    "dry-run": ("github.event_name == 'workflow_dispatch'",),
}
# Triggering-run fields that name code or refs outside the default branch.
RUNNER_LOSS_FORBIDDEN = re.compile(r"head_sha|head_commit|head_repository|pull_requests|"
                                   r"\bref:|repository:|secrets\.|github\.head_ref")


def _runner_loss_job_violations(name, job):
    violations = []
    uses = re.findall(r"(?m)^\s*(?:-\s*)?uses:\s*(\S+)", job)
    if len(uses) != 1 or not uses[0].startswith("actions/checkout@"):
        violations.append("%s runs actions other than one checkout: %s" % (name, uses))
    if ("sparse-checkout: %s\n" % RUNNER_LOSS_SCRIPT) not in job or \
            "sparse-checkout-cone-mode: false" not in job:
        violations.append("%s checks out more than the classifier" % name)
    commands = [body.strip() for body in _script_bodies(job)]
    if commands != [RUNNER_LOSS_COMMANDS.get(name)]:
        violations.append("%s runs unreviewed commands: %s" % (name, commands))
    lines = job.splitlines()
    env_blocks = [_indented_block(lines[index + 1:], 8) for index, line in enumerate(lines)
                  if line == "        env:"]
    env = _mapping(env_blocks[0]) if len(env_blocks) == 1 else {}
    if env != RUNNER_LOSS_STEP_ENV.get(name) or job.count("env:") != 1:
        violations.append("%s step environment is not the reviewed token and run ID: %s"
                          % (name, sorted(env.items())))
    gate = re.search(r"(?ms)^    if: (?:>-\n)?(.*?)^    runs-on:", job)
    expected = "${{ %s }}" % " && ".join(RUNNER_LOSS_JOB_GATES.get(name, ("<unreviewed>",)))
    if not gate or " ".join(gate.group(1).split()) != expected:
        violations.append("%s gate is not exactly %s" % (name, expected))
    return violations


def _runner_loss_violations(workflow):
    """Ways the reviewed workflow_run consumer differs from its pinned shape."""
    violations = []
    if _triggers(workflow) != RUNNER_LOSS_TRIGGERS:
        violations.append("runner-loss triggers changed: %s" % sorted(_triggers(workflow)))
    if not re.search(r'(?m)^    workflows: \["CI", "Editor WASM"\]\n    types: \[completed\]\n'
                     r'    branches: \[main\]$', workflow):
        violations.append("runner-loss workflow_run filter changed")
    violations.extend("runner-loss workflow reads %r" % match.group(0)
                      for match in RUNNER_LOSS_FORBIDDEN.finditer(workflow))
    if [line for line in _top_level_block(workflow, "env") or [] if line.strip()
            and not _is_comment(line)] != RUNNER_LOSS_WORKFLOW_ENV:
        violations.append("runner-loss workflow environment changed")
    jobs = _jobs(workflow)
    if set(jobs) != set(RUNNER_LOSS_COMMANDS):
        violations.append("runner-loss jobs changed: %s" % sorted(jobs))
    for name, job in sorted(jobs.items()):
        violations.extend(_runner_loss_job_violations(name, job))
    return violations


def _privileged_trigger_violations(path, workflow):
    events = _triggers(workflow) & PRIVILEGED_TRIGGERS
    if path == RUNNER_LOSS_WORKFLOW and events == {"workflow_run"}:
        return _runner_loss_violations(workflow)
    return sorted(events)


def _privileged_trigger_check(files, path):
    return _privileged_trigger_violations(path, files[path])


def _lane_config_flags(name):
    """The flags of a CI lane config in .bazelrc, with nested lane configs expanded.

    A lane config is one defined only on `test:` lines (fuzz-lane*, asan-lane*, ubsan-lane,
    ubsan-corpus*, asan-geode-pr). Build configs it names (asan, ubsan, asan-fuzzer, fuzz-soak,
    geode, text-full) stay as `--config=<name>`.
    """
    lines = {}
    for line in _read(".bazelrc").splitlines():
        match = re.match(r"^(build|test|common):(\S+)\s+(.*)$", line)
        if match:
            lines.setdefault(match.group(2), []).append((match.group(1), match.group(3)))

    def is_lane(config):
        return config in lines and all(command == "test" for command, _ in lines[config])

    def expand(config):
        if not is_lane(config):
            raise AssertionError("no .bazelrc lane config named " + config)
        flags = []
        for _, text in lines[config]:
            for flag in shlex.split(text):
                nested = re.fullmatch(r"--config=(\S+)", flag)
                flags.extend(expand(nested.group(1)) if nested and is_lane(nested.group(1))
                             else [flag])
        return flags

    return expand(name)


def _read(path):
    resolved = runfiles.Create().Rlocation("donner/%s" % path)
    with open(resolved, encoding="utf-8") as handle:
        return handle.read()


def _read_supply_chain_files():
    resolver = runfiles.Create()
    main = Path(resolver.Rlocation("donner/.github/workflows/main.yml"))
    github = main.parent.parent
    paths = sorted((github / "workflows").glob("*.y*ml"))
    paths.extend(sorted((github / "actions").glob("*/action.y*ml")))
    return {
        str(path.relative_to(github.parent)): path.read_text(encoding="utf-8")
        for path in paths
    }


_REMOTE_EXECUTION_INPUT = re.compile(r"(?m)^\s+remote-execution: (.*)$")


def _workflow_steps(files):
    """((workflow, job, step name), step text) for every step of every workflow."""
    for path, text in files.items():
        if not path.startswith(".github/workflows/"):
            continue
        for job, body in _jobs(text).items():
            for step in re.split(r"(?m)^      - ", body)[1:]:
                name = re.match(r"name: (.*)", step)
                yield (path, job, name.group(1).strip() if name else None), step


def _step_body(workflow, name):
    marker = "      - name: %s\n" % name
    if marker not in workflow:
        raise AssertionError("workflow step not found: %s" % name)
    body = workflow.split(marker, 1)[1]
    next_step = re.search(r"^      - name:", body, re.MULTILINE)
    return body[: next_step.start()] if next_step else body


def _run_bodies(workflow):
    lines = workflow.splitlines()
    bodies = []
    index = 0
    while index < len(lines):
        match = re.match(r"^(\s*)run:\s*\|", lines[index])
        if not match:
            index += 1
            continue
        indentation = len(match.group(1))
        index += 1
        body = []
        while index < len(lines):
            line = lines[index]
            if line and len(line) - len(line.lstrip()) <= indentation:
                break
            body.append(line)
            index += 1
        bodies.append("\n".join(body))
    return bodies


def _workflows(files):
    return {path: text for path, text in files.items() if path.startswith(".github/workflows/")}


def _is_comment(line):
    return line.lstrip().startswith("#")


def _indented_block(lines, indent):
    """The lines that follow a mapping key, up to the next line at or above its indentation.

    Comment-only lines never end a block, whatever their column.
    """
    block = []
    for line in lines:
        if line.strip() and not _is_comment(line) and len(line) - len(line.lstrip()) <= indent:
            break
        block.append(line)
    return block


def _top_level_block(text, key):
    """A top-level key's inline value as one entry, its block lines, or None when absent."""
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if line.startswith(key + ":"):
            inline = line.split(":", 1)[1].split("#", 1)[0].strip()
            return [inline] if inline else _indented_block(lines[index + 1:], 0)
    return None


def _triggers(workflow):
    block = _top_level_block(workflow, "on") or []
    if len(block) == 1 and not block[0].startswith(" "):
        return set(re.findall(r"[a-z_]+", block[0]))
    return {match.group(1) for match in (re.match(r"^  ([a-z_]+):", line) for line in block)
            if match}


def _flow_mapping(text):
    """Whether a YAML value starts a flow mapping, ignoring a trailing comment."""
    return text.split("#", 1)[0].strip().startswith("{")


def _paths_entries(body):
    """The entries of a trigger body's `paths:` filter, or None when it has no such filter.

    An inline value is kept as one entry, so a flow-style list never matches a single path.
    """
    for offset, entry in enumerate(body):
        key, _, inline = entry.split("#", 1)[0].partition(":")
        if key == "    paths":
            items = [item.strip()[1:].strip() for item in _indented_block(body[offset + 1:], 4)
                     if item.strip().startswith("- ")]
            values = [inline.strip()] if inline.strip() else items
            return [value.strip("\"'") for value in values]
    return None


def _trigger_paths(workflow):
    """Each trigger's `paths:` entries, by trigger name; a trigger without the filter is absent.

    A flow mapping for `on:` or a trigger, which could hide a filter, raises instead.
    """
    block = _top_level_block(workflow, "on") or []
    if block and _flow_mapping(block[0]):
        raise ValueError("unsupported flow mapping for on:")
    paths = {}
    for index, line in enumerate(block):
        trigger = re.match(r"^  ([a-z_]+):(.*)$", line)
        if trigger and _flow_mapping(trigger.group(2)):
            raise ValueError("unsupported flow mapping for trigger %s" % trigger.group(1))
        entries = _paths_entries(_indented_block(block[index + 1:], 2)) if trigger else None
        if entries is not None:
            paths[trigger.group(1)] = entries
    return paths


def _jobs(workflow):
    """Each job's body, keyed by job ID.

    The checks below read job keys at four columns and steps at six. A workflow laid out any
    other way raises instead of silently yielding no jobs.
    """
    jobs = {}
    name = None
    for line in _top_level_block(workflow, "jobs") or []:
        if not line.strip() or _is_comment(line):
            if name is not None:
                jobs[name].append(line)
            continue
        indent = len(line) - len(line.lstrip())
        header = re.match(r"^  ([A-Za-z0-9_-]+):\s*(?:#.*)?$", line)
        if header:
            name = header.group(1)
            jobs[name] = []
        elif name is None or indent < 4:
            raise AssertionError("unexpected jobs layout: %r" % line)
        else:
            jobs[name].append(line)
    return {name: "\n".join(lines) for name, lines in jobs.items()}


def _mapping(lines):
    entries = (line.split("#", 1)[0].partition(":") for line in lines)
    return {key.strip(): value.strip() for key, _, value in entries if key.strip()}


def _permissions(text, indent):
    """The `permissions:` mapping at this indentation, `{"*": value}` inline, or None."""
    lines = text.splitlines()
    key = " " * indent + "permissions:"
    for index, line in enumerate(lines):
        if line.startswith(key):
            inline = line[len(key):].split("#", 1)[0].strip()
            if inline:
                return {} if inline == "{}" else {"*": inline}
            return _mapping(_indented_block(lines[index + 1:], indent))
    return None


def _steps(job, indent=6):
    parts = re.split(r"(?m)^%s- " % (" " * indent), job)[1:]
    return ["\n".join([part.splitlines()[0],
                       *_indented_block(part.splitlines()[1:], indent)]) for part in parts]


def _step_values(step, key, indent=10):
    """A step `with:` value as entries: one inline value, or each line of a block scalar."""
    match = re.search(r"(?m)^%s%s:[ \t]*(.*)$" % (" " * indent, re.escape(key)), step)
    if not match:
        return []
    value = match.group(1).strip()
    if value not in ("|", "|-", ">", ">-"):
        return [value]
    block = _indented_block(step[match.end():].splitlines()[1:], indent)
    return [line.strip() for line in block if line.strip()]


def _script_bodies(text):
    """Every shell, github-script and retry-action command body, block or inline."""
    lines = text.splitlines()
    bodies = []
    for index, line in enumerate(lines):
        match = re.match(r"^(\s*)(?:-\s+)?(?:run|script|command):[ \t]*(.*)$", line)
        if not match:
            continue
        value = match.group(2).strip()
        if value in ("|", "|-", ">", ">-"):
            value = "\n".join(_indented_block(lines[index + 1:], len(match.group(1))))
        bodies.append(value)
    return bodies


def _expression_end(text, index):
    """Find the first unquoted close delimiter with one forward scan."""
    quoted = False
    while index < len(text):
        if text[index] == "'":
            if quoted and text[index:index + 2] == "''":
                index += 2
                continue
            quoted = not quoted
        elif not quoted and text[index:index + 2] == "}}":
            return index
        index += 1
    return None


def _expressions(text):
    """Ignore quoted close delimiters without backtracking on malformed expressions."""
    cursor = 0
    while (start := text.find("${{", cursor)) != -1:
        end = _expression_end(text, start + 3)
        if end is None:
            # Fail closed even in comments: a marker must not hide later reads.
            raise ValueError("unterminated expression marker")
        # Conservatively reject hash-before-marker text, without parsing YAML or shell.
        line_start = text.rfind("\n", 0, start) + 1
        if "#" in text[line_start:start]:
            raise ValueError("ambiguous comment expression marker")
        yield re.fullmatch(r"\$\{\{(.*)\}\}", text[start:end + 2], re.DOTALL)
        cursor = end + 2


def _canonical_context(expression):
    """Give literal index and property access the same spelling for policy checks."""
    indexed = re.sub(r"\s*\[\s*(['\"])([A-Za-z_][A-Za-z0-9_]*)\1\s*\]",
                     r".\2", expression)
    # Resolve literal index keys first, then exclude harmless expression strings.
    unquoted = re.sub(r"'(?:''|[^'])*'", "''", indexed)
    return re.sub(r"\s*\.\s*", ".", unquoted)


def _secret_reads(text):
    names = set()
    for match in _expressions(text):
        expression = _canonical_context(match.group(1))
        names.update(name.upper() for name in
                     re.findall(r"\bsecrets\.([A-Za-z0-9_]+)", expression, re.IGNORECASE))
        if re.search(r"\bsecrets(?:\s*\[|\.\*)", expression, re.IGNORECASE):
            names.add("<unreviewable secret reference>")
        if re.search(r"(?<![\w.])secrets\b(?!\s*[.\[])", expression, re.IGNORECASE):
            names.add("<whole secret object>")
    return names


def _untrusted_script_contexts(text):
    found = []
    for body in _script_bodies(text):
        for match in _expressions(body):
            expression = _canonical_context(match.group(1)).lower()
            found.extend("inputs." if context.startswith("inputs") else context
                         for context in UNTRUSTED_CONTEXT.findall(expression)
                         if context not in SCRIPT_SAFE_EVENT_FIELDS)
    return found


def _token_scope_violations(path, workflow):
    violations = []
    top = _permissions(workflow, 0)
    if top is not None and (set(top.values()) - {"read", "none"} or "*" in top):
        violations.append("%s grants more than read scopes to every job" % path)
    for name, job in _jobs(workflow).items():
        scopes = _permissions(job, 4)
        if scopes is None and top is None:
            violations.append("%s:%s inherits the repository's default token" % (path, name))
            continue
        scopes = scopes or {}
        writes = {scope for scope, level in scopes.items() if level not in ("read", "none")}
        if "*" in scopes or writes != WRITE_SCOPES.get((path, name), set()):
            violations.append("%s:%s requests write scopes %s" % (path, name, sorted(writes)))
    return violations


def _step_identity_value(step, key):
    match = re.search(r"(?m)^(?:        )?%s:[ \t]*(.*)$" % re.escape(key), step)
    return match.group(1).strip() if match else None


def _secret_reference_counts(text):
    counts = Counter()
    for match in _expressions(text):
        counts.update(_secret_reads(match.group()))
    return counts


def _unscoped_secret_reads(workflow):
    steps = Counter()
    for job in _jobs(workflow).values():
        for step in _steps(job):
            steps.update(_secret_reference_counts(step))
    return _secret_reference_counts(workflow) - steps


def _step_secret_consumers(path, workflow):
    for name, job in _jobs(workflow).items():
        for step in _steps(job):
            secrets = _secret_reads(step)
            if secrets:
                identity = (path, name, _step_identity_value(step, "name"),
                            _step_identity_value(step, "id"),
                            _step_identity_value(step, "uses") or "<run>")
                yield identity, secrets


def _call_secrets(job):
    """A calling job's `secrets:` block as {name: value}, `{"*": value}` inline, or None."""
    lines = job.splitlines()
    for index, line in enumerate(lines):
        if line.startswith("    secrets:"):
            inline = line.split(":", 1)[1].split(" #", 1)[0].strip()
            if inline:
                return {"*": inline}
            return _mapping(_indented_block(lines[index + 1:], 4))
    return None


def _forwarded_secrets(names):
    """The one reviewed spelling of a `secrets:` block that forwards each name as itself."""
    return {name: "${{ secrets.%s }}" % name for name in names}


def _block_after(lines, pattern, indent):
    """The block under the first line matching `pattern`, or None when no line matches."""
    starts = [index for index, line in enumerate(lines) if re.match(pattern, line)]
    return _indented_block(lines[starts[0] + 1:], indent) if starts else None


def _declared_call_secrets(workflow):
    """The secret names a workflow declares under `on.workflow_call.secrets`, or None."""
    call = _block_after(_top_level_block(workflow, "on") or [], r"^  workflow_call:\s*$", 2)
    if call is None:
        return None
    names = _block_after(call, r"^    secrets:\s*$", 4) or []
    return {match.group(1) for match in
            (re.match(r"^      ([A-Za-z0-9_]+):", line) for line in names) if match}


def _reviewed_forwarded_reads(path, workflow):
    """Secret reads of this workflow's reviewed calls, when each forwards exactly its list."""
    reads = Counter()
    for (caller, name), (_, secrets) in REVIEWED_WORKFLOW_CALLS.items():
        job = _jobs(workflow).get(name) if caller == path else None
        if job is not None and _call_secrets(job) == _forwarded_secrets(secrets):
            reads.update(secrets)
    return reads


def _workflow_calls(workflows):
    """Every job that calls a reusable workflow: (path, job) to (callee, forwarded secrets)."""
    return {(path, name): (_job_key(job, "uses").strip("'\""), _call_secrets(job))
            for path, workflow in workflows.items()
            for name, job in _jobs(workflow).items() if _job_key(job, "uses")}


def _workflow_call_problem(workflows, where, call, reviewed):
    """Why one reviewed call differs from its review, or None."""
    callee, forwarded = call
    reviewed_callee, secrets = reviewed
    if callee != reviewed_callee:
        return "%s calls %s instead of %s" % (where, callee, reviewed_callee)
    if forwarded != _forwarded_secrets(secrets):
        return "%s forwards secrets other than %s" % (where, sorted(secrets))
    declared = _declared_call_secrets(
        workflows.get(".github/workflows/" + callee.rsplit("/", 1)[-1], ""))
    if declared != set(secrets):
        return "%s callee declares workflow_call secrets %s" % (
            where, sorted(declared) if declared is not None else None)
    return None


def _workflow_call_violations(workflows):
    """Every reusable workflow call that is unreviewed or forwards an unreviewed secret."""
    calls = _workflow_calls(workflows)
    violations = []
    if set(calls) != set(REVIEWED_WORKFLOW_CALLS):
        violations.append("reusable workflow calls changed: %s" % sorted(calls))
    for key in sorted(set(calls) & set(REVIEWED_WORKFLOW_CALLS)):
        problem = _workflow_call_problem(workflows, "%s:%s" % key, calls[key],
                                         REVIEWED_WORKFLOW_CALLS[key])
        if problem:
            violations.append(problem)
    return violations


def _secret_use_violations(workflows):
    records = []
    violations = []
    for path, workflow in workflows.items():
        if (re.search(r"secrets:\s*inherit", workflow, re.IGNORECASE) or
                "<whole secret object>" in _secret_reads(workflow)):
            violations.append("%s forwards every secret" % path)
        if _unscoped_secret_reads(workflow) - _reviewed_forwarded_reads(path, workflow):
            violations.append("%s exposes a secret beyond one step" % path)
        records.extend(_step_secret_consumers(path, workflow))
    uses = dict(records)
    if len(uses) != len(records):
        violations.append("duplicate secret consumer steps")
    if uses != SECRET_USES:
        violations.append("secret uses changed: %s" %
                          sorted(uses.items(), key=lambda item: repr(item[0])))
    return violations


def _job_key(job, key):
    match = re.search(r"(?m)^    %s:(.*)$" % key, job)
    return match.group(1).split(" #", 1)[0].strip() if match else ""


def _labels(text):
    return [label.strip().strip("'\"") for label in text.split(",") if label.strip()]


def _without_computed_exclusions(block):
    """A matrix block without the computed values of its `exclude:` entries.

    An exclusion can only remove a combination the literal entries already define, never add a
    runner label, so its values may be computed from an earlier job's output. Every other line,
    including literal exclusions, is kept and read: a superset of labels is the safe reading.
    """
    kept = []
    excluding = False
    for line in block:
        if len(line) - len(line.lstrip()) <= 8:
            excluding = re.match(r"^        exclude:\s*$", line) is not None
        if not (excluding and "${{" in line):
            kept.append(line)
    return kept


def _unreadable_matrix_line(line):
    """Expressions, flow mappings and YAML anchors or aliases can all supply a value this
    line-based reading would miss."""
    return "{" in line or re.search(r"(?:^|[\s\[,-])[&*][A-Za-z0-9_]", line) is not None


def _matrix_values(job, key):
    """Every literal value of one matrix key, or None when the matrix is not fully literal."""
    lines = job.splitlines()
    starts = [index for index, line in enumerate(lines) if re.match(r"^      matrix:", line)]
    if len(starts) != 1 or lines[starts[0]].split(":", 1)[1].split("#", 1)[0].strip():
        return None
    block = [line for line in _indented_block(lines[starts[0] + 1:], 6)
             if line.strip() and not _is_comment(line)]
    block = _without_computed_exclusions(block)
    if any(_unreadable_matrix_line(line) for line in block):
        return None
    values = []
    for line in block:
        match = re.match(r"^\s*(?:-\s+)?%s:\s*(.+?)\s*(?:#.*)?$" % re.escape(key), line)
        if match:
            value = match.group(1)
            values.extend(_labels(value[1:-1]) if value.startswith("[") else [value.strip("'\"")])
    return values or None


def _block_runner(lines, start):
    """A block `runs-on`: a label list, or a runner group mapping (never a hosted image)."""
    block = [line.strip() for line in _indented_block(lines[start + 1:], 4)
             if line.strip() and not _is_comment(line)]
    if block and all(line.startswith("- ") for line in block):
        return [{line[2:].strip().strip("'\"") for line in block}]
    return [{"<runner group>"}]


def _expression_runner(path, name, job, inline):
    """Label sets for an expression `runs-on`, or a reason it cannot be reviewed."""
    matrix = re.fullmatch(r"\$\{\{\s*matrix\.([A-Za-z0-9_-]+)\s*\}\}", inline)
    if matrix:
        values = _matrix_values(job, matrix.group(1))
        if values is None:
            return [], "%s:%s selects its runner from a non-literal matrix" % (path, name)
        return [{value} for value in values], None
    reviewed = REVIEWED_RUNNER_EXPRESSIONS.get((path, name))
    if not reviewed or reviewed[0] != inline:
        return [], "%s:%s selects its runner from an unreviewed expression" % (path, name)
    return [{label} for label in reviewed[1]], None


def _runner_label_sets(path, name, job):
    """Each label set this job can request, or a reason the selection cannot be reviewed."""
    lines = job.splitlines()
    starts = [index for index, line in enumerate(lines) if line.startswith("    runs-on:")]
    if not starts:
        return [], None
    inline = _job_key(job, "runs-on")
    if re.search(r"vars\.|inputs\.|github\.event|fromJSON", inline, re.IGNORECASE):
        return [], "%s:%s selects its runner from mutable input" % (path, name)
    if not inline:
        return _block_runner(lines, starts[0]), None
    if inline.startswith("["):
        return [set(_labels(inline[1:-1]))], None
    if "${{" in inline:
        return _expression_runner(path, name, job, inline)
    return [{inline.strip("'\"")}], None


def _is_hosted(labels):
    return len(labels) == 1 and next(iter(labels)) in HOSTED_IMAGES


def _runs_self_hosted(path, name, job):
    sets, _ = _runner_label_sets(path, name, job)
    return any(not _is_hosted(labels) for labels in sets)


def _runner_violation(path, name, job):
    """Why this job's runner selection is unsafe, or None."""
    sets, problem = _runner_label_sets(path, name, job)
    if problem:
        return problem
    if all(_is_hosted(labels) for labels in sets):
        return None
    prefix = SELF_HOSTED_GATE_PREFIX.replace(
        "${{ ", "${{ " + SELF_HOSTED_STATUS_PREFIX.get((path, name), ""), 1)
    gate = prefix + "needs.gatekeeper.outputs.%s == 'true'%s }}" % (
        SELF_HOSTED_JOBS.get((path, name)), SELF_HOSTED_NARROWING.get((path, name), ""))
    needs = _job_key(job, "needs").strip("[]")
    admitted = ((path, name) in SELF_HOSTED_JOBS and gate == _job_key(job, "if")
                and "gatekeeper" in _labels(needs))
    if not admitted:
        return "%s:%s reaches a self-hosted runner without the trusted gate" % (path, name)
    return None


def _self_hosted_violations(workflows):
    jobs = [(path, name, job) for path, workflow in workflows.items()
            for name, job in _jobs(workflow).items()]
    violations = [violation for violation in (_runner_violation(*job) for job in jobs)
                  if violation]
    seen = {(path, name) for path, name, job in jobs if _runs_self_hosted(path, name, job)}
    if seen != set(SELF_HOSTED_JOBS):
        violations.append("self-hosted jobs changed: %s" % sorted(seen))
    return violations


def _upload_paths(job):
    """The `path` entries of every step that publishes an artifact or saves a cache directly."""
    return [_step_values(step, "path") for step in _steps(job)
            if re.search(r"uses: (?:actions/upload-artifact@|actions/upload-pages-artifact@|"
                         r"actions/cache@|actions/cache/save@)", step)]


UPLOAD_ACTION = re.compile(r"uses: (?:actions/upload-artifact@|actions/upload-pages-artifact@)")


def _uploading_local_actions(files):
    return {"./" + path.rsplit("/", 1)[0] for path, text in files.items()
            if path.startswith(".github/actions/") and UPLOAD_ACTION.search(text)}


def _unsafe_upload(entry, self_hosted):
    if entry.startswith("!"):
        return False
    if UPLOADED_EVENT_LOG.search(entry) or entry in WORKSPACE_ROOTS:
        return True
    leaf = entry.rstrip("/").rsplit("/", 1)[-1]
    return self_hosted and (entry.endswith("/") or leaf not in SELF_HOSTED_UPLOAD_FILES)


# Upload paths taken from a step output in a job that configures BuildBuddy, each reviewed with
# the one value its producing step writes: a named subdirectory of runner.temp, never the root
# that also holds the BuildBuddy key directory. The producing step is exactly one call of a
# reviewed script, which assigns that value once from the runner's temporary directory and writes
# the output once, from that assignment. Keyed like the upload; the value is (producer command,
# script, reviewed assignment).
EDITOR_WASM_STAGE = "tools/ci/editor_wasm_stage.py"
REVIEWED_OUTPUT_UPLOADS = {
    (".github/workflows/editor_wasm.yml", "build",
     "${{ steps.stage-package.outputs.handoff_dir }}"):
        ("python3 %s package" % EDITOR_WASM_STAGE, EDITOR_WASM_STAGE,
         'handoff_dir = f"{runner_temp}/editor-wasm-package-geode"'),
    (".github/workflows/editor_wasm.yml", "build",
     "${{ steps.stage-candidate.outputs.candidate_dir }}"):
        ("python3 %s candidate" % EDITOR_WASM_STAGE, EDITOR_WASM_STAGE,
         'candidate_dir = f"{runner_temp}/donner-editor-wasm-geode-{environ[\'GITHUB_SHA\']}"'),
}
# The one source of `runner_temp` in a reviewed script.
REVIEWED_RUNNER_TEMP = 'runner_temp = environ["RUNNER_TEMP"]'
# A reviewed script opens the step output file only in its `_write_output` helper.
REVIEWED_OUTPUT_FILE = 'environ["GITHUB_OUTPUT"]'


def _writes_outputs_only_through_helper(text):
    """Whether the script reads GITHUB_OUTPUT exactly once, inside `_write_output`; the only
    other mention allowed is the name in its required-environment list."""
    helper = next((part for part in re.split(r"(?m)^def ", text)
                   if part.startswith("_write_output(")), "")
    return (text.count(REVIEWED_OUTPUT_FILE) == 1 and helper.count(REVIEWED_OUTPUT_FILE) == 1 and
            len(re.findall(r"""["']GITHUB_OUTPUT["']""", text)) == 2)


def _reviewed_script(files, script):
    """A reviewed script's text: the test's replacement in `files`, else the checked-in one."""
    return files[script] if script in files else _read(script)


def _reviewed_output_upload(path, name, job, entry, files):
    """Whether an upload path is a reviewed step output whose producer writes it exactly once,
    from the reviewed value."""
    reviewed = REVIEWED_OUTPUT_UPLOADS.get((path, name, entry))
    match = re.fullmatch(r"\$\{\{ steps\.([A-Za-z0-9_-]+)\.outputs\.([A-Za-z0-9_]+) \}\}", entry)
    if reviewed is None or not match:
        return False
    command, script, assignment = reviewed
    producers = [step for step in _steps(job) if _step_identity_value(step, "id") == match.group(1)]
    if len(producers) != 1:
        return False
    # The step runs the command alone: one `run:` line, with no continuation lines.
    lines = producers[0].splitlines()
    runs = [index for index, line in enumerate(lines) if line.startswith("        run:")]
    if (len(runs) != 1 or lines[runs[0]] != "        run: " + command or
            any(line.startswith("         ") for line in lines[runs[0] + 1:runs[0] + 2])):
        return False
    variable = match.group(2)
    text = _reviewed_script(files, script)
    function = next((part for part in re.split(r"(?m)^def ", text) if assignment in part), "")
    names = re.findall(r"_write_output\(environ, ([^,]*),", text)
    writes = re.findall(r'_write_output\(environ, "%s", ([^)]*)\)' % variable, text)
    return (_writes_outputs_only_through_helper(text) and
            all(re.fullmatch(r'"[a-z_]+"', name) for name in names) and
            writes == [variable] and function.count(assignment) == 1 and
            len(re.findall(r"(?m)^\s*%s = " % variable, text)) == 1 and
            function.count(REVIEWED_RUNNER_TEMP) == 1 and
            len(re.findall(r"(?m)^\s*runner_temp = ", function)) == 1)


def _reaches_buildbuddy_credential(entry):
    """Whether an upload path can reach runner.temp/buildbuddy, where the key file lives."""
    entry = entry.lstrip("!").strip().strip("'\"")
    temp = re.match(r"(?:\$\{\{ runner\.temp \}\}|\$\{?RUNNER_TEMP\}?|/home/runner/work/_temp|"
                    r"/Users/runner/work/_temp)(/.*)?$", entry)
    if temp:
        first = (temp.group(1) or "/").lstrip("/").split("/", 1)[0]
        return first in ("", "buildbuddy") or any(char in first for char in "*?[") or \
            ".." in entry
    if entry.startswith("~"):
        # The runner's temporary directory lives under its home directory's `work` tree.
        first = entry[1:].lstrip("/").split("/", 1)[0]
        return first in ("", "work") or any(char in first for char in "*?[")
    return entry in WORKSPACE_ROOTS or entry.startswith(("/", "..", "$")) or \
        "buildbuddy" in entry.lower() or "*" in entry.split("/", 1)[0]


def _credential_upload_violations(path, name, job, files):
    """The BuildBuddy key lives under runner.temp/buildbuddy, so a job that configures it may
    upload only paths that can reach neither that directory nor the runner.temp root."""
    return ["%s:%s uploads %s after writing a cache credential" % (path, name, entry)
            for entries in _upload_paths(job) for entry in entries
            if not _reviewed_output_upload(path, name, job, entry.strip(), files)
            and _reaches_buildbuddy_credential(entry)]


def _upload_violations(path, name, job, uploading_actions, files):
    violations = []
    local = set(re.findall(r"uses: (\./\.github/actions/[A-Za-z0-9_-]+)", job))
    # The reviewed collector copies only Bazel test logs, so it stays usable in jobs that
    # configure a cache credential.
    if BUILDBUDDY_ACTION in job:
        violations.extend(_credential_upload_violations(path, name, job, files))
    for action in sorted(local & uploading_actions - REVIEWED_UPLOAD_ACTIONS):
        violations.append("%s:%s uploads through unreviewed action %s" % (path, name, action))
    self_hosted = (path, name) in SELF_HOSTED_JOBS
    if self_hosted and CACHE_SAVE.search(job):
        violations.append("%s:%s saves an Actions cache from a self-hosted runner" % (path, name))
    violations.extend("%s:%s uploads %s" % (path, name, entry)
                      for entries in _upload_paths(job) for entry in entries
                      if _unsafe_upload(entry, self_hosted))
    return violations


def _reviewed_action_upload_violations(files):
    problems = []
    for action in REVIEWED_UPLOAD_ACTIONS:
        path = action.removeprefix("./") + "/action.yml"
        uploads = [_step_values(step, "path", indent=8) for step in _steps(files[path], indent=4)
                   if UPLOAD_ACTION.search(step)]
        if uploads != [["${{ runner.temp }}/bazel-test-artifacts"]]:
            problems.append("%s changes the reviewed collected-artifact upload path" % path)
    return problems


def _artifact_upload_violations(files):
    uploading_actions = _uploading_local_actions(files)
    return _reviewed_action_upload_violations(files) + [
        violation for path, workflow in _workflows(files).items()
        for name, job in _jobs(workflow).items()
        for violation in _upload_violations(path, name, job, uploading_actions, files)]


# A publication or record-assembly workflow promotes retained bytes; it never builds them. These
# commands and actions compile, package, generate or upload a site.
BUILD_COMMAND = re.compile(
    r"(?:^|[\s;&|(])(bazel|bazelisk|cmake|make|ninja|npm|npx|node|doxygen|cargo|rustc|emcc|"
    r"clang\+\+|g\+\+|gcc|tools/build_docs\.sh|tools/doxygen\.sh)(?=[\s;&|)]|$)", re.MULTILINE)
BUILD_ACTIONS = ("bazel-contrib/setup-bazel@", "actions/setup-node@", "upload-pages-artifact@",
                 "./.github/actions/cache-bazel-actions", "bazel-contrib/publish-to-bcr@")
CANDIDATE_WORKFLOW = ".github/workflows/release_candidate.yml"
CANDIDATE_INPUTS = ("candidate_commit", "preflight_run", "editor_run", "docs_run", "ci_run",
                    "coverage_run", "fuzz_run", "sanitizers_run", "security_review_comment",
                    "supply_chain_review_comment")
RELEASE_CLI_ASSETS = tuple(
    "donner-svg_%s%s" % (platform, suffix) for platform in ("linux_x86_64", "darwin_arm64")
    for suffix in ("", ".sha256", ".provenance", ".bazel.lock", ".NOTICE.txt"))


def _build_steps(workflow):
    found = [match.group(1) for body in _script_bodies(workflow)
             for match in BUILD_COMMAND.finditer(body)]
    return found + [action for action in BUILD_ACTIONS if action in workflow]


def _named_step(job, name):
    steps = [step for step in _steps(job) if _step_identity_value(step, "name") == name]
    return steps[0] if len(steps) == 1 else ""


def _continued_arguments(script, command):
    """The arguments of one backslash-continued command, one entry per line."""
    if command not in script:
        return []
    tail = script.split(command, 1)[1]
    arguments = []
    for line in tail.splitlines()[1:]:
        arguments.append(line.strip().rstrip("\\").strip())
        if not line.rstrip().endswith("\\"):
            break
    return arguments


def _failed(checks):
    """The message of every policy check whose condition failed."""
    return [message for passed, message in checks if not passed]


RELEASE_STEPS = ("Reverify the candidate record and stage its release files",
                 "Verify the candidate record attestation",
                 "Verify preflight build attestations",
                 "Upload verified release artifacts without replacement")
RELEASE_FORBIDDEN = ("actions/upload-artifact@", "actions/download-artifact@",
                     "actions/attest-build-provenance@", "id-token: write", "--lockfile_mode",
                     "Release-Candidate-Preflight", "select-preflight")
RELEASE_RECORD_ARGUMENTS = ('--record-run-id "$RECORD_RUN_ID"', '--record-attempt "$RECORD_ATTEMPT"',
                            '--record-artifact-id "$RECORD_ARTIFACT_ID"',
                            '--record-sha256 "$RECORD_SHA256"')
# The release source check runs in YAML before any repository code: that code cannot vouch for
# itself, so the check must stay out of it.
RELEASE_SOURCE_STEP = "Reverify release source"
RELEASE_SOURCE_CHECKS = ('test "$(git rev-parse HEAD)" = "$GITHUB_SHA"',
                         'test "$(git rev-list -n 1 "refs/tags/${RELEASE_TAG}")" = "$GITHUB_SHA"')
RELEASE_CLI_STEP = "Verify immutable artifacts"
RELEASE_CLI_VERIFY = ("python3 -B -m tools.release_cli verify-release",
                      '--release "$RUNNER_TEMP/release"', '--commit "$GITHUB_SHA"',
                      '--run-id "$SOURCE_RUN_ID"', '--attempt "$SOURCE_ATTEMPT"',
                      "SOURCE_RUN_ID: ${{ steps.candidate.outputs.preflight_run_id }}",
                      "SOURCE_ATTEMPT: ${{ steps.candidate.outputs.preflight_attempt }}")


def _release_asset_paths(assets):
    return ['"$RUNNER_TEMP"/release/%s/%s' % ("cli-linux" if "linux" in asset else "cli-macos",
                                              asset) for asset in assets]


def _release_shape_violations(release):
    signer = "--signer-workflow jwmcglynn/donner/.github/workflows/"
    return _failed([
        (_triggers(release) == {"release"} and "types: [published]" in release,
         "release runs on an event other than a published release"),
        (not _build_steps(release), "release builds: %s" % _build_steps(release)),
        (release.count("gh attestation verify") == 2 and
         release.count(signer + "release_candidate.yml") == 1 and
         release.count('--source-digest "$GITHUB_SHA" --source-ref refs/heads/main') == 1 and
         release.count(signer + "bcr_preflight.yml") == 1,
         "release does not verify the record and preflight attestations"),
        (not any("${{" in body for body in _run_bodies(release)),
         "release run blocks must use quoted environment values"),
    ]) + ["release contains %s" % item for item in RELEASE_FORBIDDEN if item in release]


def _release_step_violations(jobs):
    publication = jobs["publish-release-artifacts"]
    names = [_step_identity_value(step, "name") for step in _steps(publication)]
    stage = _named_step(publication, RELEASE_STEPS[0])
    reverified = "python3 -B -m tools.release_candidate release" in stage and all(
        argument in stage for argument in RELEASE_RECORD_ARGUMENTS)
    first_run = next((step for step in _steps(publication) if "run:" in step), "")
    first_steps = _steps(publication)[:2]
    cli = _named_step(publication, RELEASE_CLI_STEP)
    return _failed([
        (jobs["resolve-candidate"].count("python3 -B -m tools.bcr_release select-record") == 1,
         "release does not select the record named by the release body"),
        ("needs: resolve-candidate" in publication, "publication does not wait for record selection"),
        ([name for name in names if name in RELEASE_STEPS] == list(RELEASE_STEPS) and
         names[-1:] == [RELEASE_STEPS[-1]],
         "release does not verify the record before its final upload step"),
        (reverified, "release does not re-verify the named record"),
        # Only the checkout may run before the source check: no action or script from the tag.
        (_step_identity_value(first_run, "name") == RELEASE_SOURCE_STEP and
         all(check in first_run for check in RELEASE_SOURCE_CHECKS) and
         len(first_steps) == 2 and "uses: actions/checkout@" in first_steps[0] and
         first_steps[1] == first_run,
         "release runs code before checking the checkout is the release commit"),
        # With the upload as the final step (checked above), any earlier step precedes it.
        (all(argument in cli for argument in RELEASE_CLI_VERIFY) and RELEASE_CLI_STEP in names[:-1],
         "release does not verify the staged CLI files before upload"),
    ])


def _release_file_violations(publication):
    attested = _continued_arguments(_named_step(publication, RELEASE_STEPS[2]), "for subject in")
    signed = [asset for asset in RELEASE_CLI_ASSETS if asset.endswith(("64", ".bazel.lock",
                                                                        ".NOTICE.txt"))]
    uploaded = _continued_arguments(_named_step(publication, RELEASE_STEPS[3]),
                                    "python3 tools/release_artifact_publisher.py")
    expected_uploaded = _release_asset_paths(RELEASE_CLI_ASSETS) + [
        '"$RUNNER_TEMP"/release/source/*', '"$RUNNER_TEMP"/release/donner-*.candidate.json']
    return _failed([
        (attested == _release_asset_paths(signed) + ['"$RUNNER_TEMP"/release/source/*.tar.gz; do'],
         "release attestation subjects changed: %s" % attested),
        (uploaded == expected_uploaded, "release uploads files the record did not stage: %s" % uploaded),
    ])


def _release_publication_violations(release):
    """Release must publish exactly the record's verified files and build nothing."""
    jobs = _jobs(release)
    if set(jobs) != {"resolve-candidate", "publish-release-artifacts"}:
        return ["release jobs changed: %s" % sorted(jobs)]
    return (_release_shape_violations(release) + _release_step_violations(jobs) +
            _release_file_violations(jobs["publish-release-artifacts"]))


ASSEMBLY_PUBLICATIONS = ("gh release", "gh workflow", "gh pr", "/pages/deployments", "git push",
                         "release_artifact_publisher")


def _candidate_input_names(workflow):
    inputs = workflow.split("    inputs:\n", 1)[-1].split("\npermissions:", 1)[0]
    required = inputs.count("        required: true\n")
    names = re.findall(r"(?m)^      ([a-z][a-z0-9_]*):$", inputs)
    return names if required == len(names) else names + ["<optional input>"]


def _candidate_workflow_violations(workflow):
    scripts = _run_bodies(workflow)
    return _failed([
        (_triggers(workflow) == {"workflow_dispatch"},
         "assembly runs on an event other than a manual dispatch"),
        (_candidate_input_names(workflow) == list(CANDIDATE_INPUTS),
         "assembly inputs changed: %s" % _candidate_input_names(workflow)),
        (_permissions(workflow, 0) == {"contents": "read"}, "assembly widens the workflow token"),
        (not _secret_reads(workflow) and "secrets" not in workflow, "assembly reads a secret"),
        (not _build_steps(workflow), "assembly builds: %s" % _build_steps(workflow)),
        (not _untrusted_script_contexts(workflow) and not any("${{" in body for body in scripts),
         "assembly interpolates inputs into a script"),
        (not re.search(r"uses: [^\n]+@[0-9a-f]{40}", workflow) and
         "continue-on-error" not in workflow,
         "assembly pins an action by commit or ignores a failure"),
    ]) + ["assembly publishes: %s" % item for item in ASSEMBLY_PUBLICATIONS if item in workflow]


def _candidate_retention_violations(assemble):
    uploads = [step for step in _steps(assemble) if "actions/upload-artifact@" in step]
    values = [(_step_values(step, "name"), _step_values(step, "path"),
               _step_values(step, "retention-days"), _step_values(step, "if-no-files-found"))
              for step in uploads]
    expected = (["donner-release-candidate-${{ github.run_attempt }}"],
                ["${{ runner.temp }}/candidate/"], ["90"], ["error"])
    return _failed([(values == [expected], "assembly does not retain the record for 90 days")])


def _candidate_job_violations(jobs):
    assemble, attest = jobs["assemble"], jobs["attest-record"]
    guard = "    if: github.repository == 'jwmcglynn/donner' && github.ref == 'refs/heads/main'\n"
    check = attest.find("sha256sum --check --strict candidate.json.sha256")
    signer = attest.find("uses: actions/attest-build-provenance@")
    return _failed([
        (guard in assemble + "\n", "assembly can run outside main"),
        (_permissions(assemble, 4) == {"contents": "read", "actions": "read", "issues": "read"},
         "assembly job token scopes changed"),
        (_permissions(attest, 4) == {"contents": "read", "id-token": "write",
                                     "attestations": "write"},
         "attestation job token scopes changed"),
        (assemble.count("python3 -B -m tools.release_candidate assemble") == 1,
         "assembly does not run the candidate verifier"),
        ("needs: assemble" in attest and "actions/checkout@" not in attest and
         "python3" not in attest, "attestation job runs repository code or skips assembly"),
        (0 <= check < signer and
         "subject-path: ${{ runner.temp }}/candidate/candidate.json\n" in attest + "\n",
         "attestation does not sign the checked record"),
    ]) + _candidate_retention_violations(assemble)


def _candidate_assembly_violations(workflow):
    """Assembly is manual, least-privilege, secret-free, retained and attested, and builds nothing."""
    jobs = _jobs(workflow)
    if set(jobs) != {"assemble", "attest-record"}:
        return ["assembly jobs changed: %s" % sorted(jobs)]
    return _candidate_workflow_violations(workflow) + _candidate_job_violations(jobs)


BCR_SUBMISSION_WORKFLOW = ".github/workflows/publish_bcr.yml"
BCR_SUBMISSION_INPUTS = ("release_run_id", "record_sha256")
BCR_SUBMISSION_JOBS = {"validate", "promote-entry", "prepare-fork", "publish-fork"}
BCR_PLAN_STEP = "Verify the release and its candidate record and plan the submission"
BCR_PROMOTE_STEPS = ("Fetch the admission-validated entry the record names",
                     "Stage the validated entry with derived metadata",
                     "Verify and bundle the proposed entry without a fork token")
BCR_RECHECK_STEP = "Verify the exact proposed commit without a fork token"
BCR_FETCH_ENTRY = "python3 -B -m tools.bcr_release fetch-entry"
BCR_VALIDATED_ENTRY = '--validated-entry "$RUNNER_TEMP/validated-entry/entry"'
BCR_RECORD_DIGEST = "APPROVED_SOURCE_SHA256: ${{ needs.validate.outputs.source_sha256 }}"
# `bundle-entry` checks the staged change as `verify-generated` does before it commits and bundles
# it; `verify-bundle` checks the downloaded bundle is that commit, then checks the commit again.
# Both run main's publication checks from the `trusted` checkout, never the release source's.
BCR_BUNDLE_ENTRY = "PYTHONPATH=trusted python3 -B -m tools.bcr_release bundle-entry"
BCR_VERIFY_BUNDLE = "PYTHONPATH=trusted python3 -B -m tools.bcr_release verify-bundle"
BCR_PROMOTED = (("BUNDLE_SHA256: ${{ needs.promote-entry.outputs.bundle_sha256 }}",
                 '--bundle-sha256 "$BUNDLE_SHA256"'),
                ("PREPARED_BASE: ${{ needs.promote-entry.outputs.base }}",
                 '--base "$PREPARED_BASE"'),
                ("PREPARED_TREE: ${{ needs.promote-entry.outputs.tree }}",
                 '--tree "$PREPARED_TREE"'),
                ("PREPARED_COMMIT: ${{ needs.promote-entry.outputs.commit }}",
                 '--proposed-commit "$PREPARED_COMMIT"'))


def _bcr_validation_violations(validate):
    plan = _named_step(validate, BCR_PLAN_STEP)
    return _failed([
        ("python3 -B -m tools.bcr_release plan-submission" in plan and
         '--record-sha256 "$RECORD_SHA256"' in plan,
         "submission is not planned from the approved candidate record"),
        (_permissions(validate, 4) == {"contents": "read", "actions": "read",
                                       "attestations": "read"},
         "validation job token scopes changed"),
    ])


def _bcr_promotion_violations(promote, prepare):
    names = [_step_identity_value(step, "name") for step in _steps(promote)]
    bundle = _named_step(promote, BCR_PROMOTE_STEPS[2])
    recheck = _named_step(prepare, BCR_RECHECK_STEP)
    return _failed([
        ([name for name in names if name in BCR_PROMOTE_STEPS] == list(BCR_PROMOTE_STEPS),
         "promotion does not fetch, stage and verify the validated entry in order"),
        (BCR_FETCH_ENTRY in _named_step(promote, BCR_PROMOTE_STEPS[0]) and
         "tools.bcr_release stage-entry" in _named_step(promote, BCR_PROMOTE_STEPS[1]),
         "promotion does not submit the admission-validated entry"),
        (BCR_BUNDLE_ENTRY in bundle and BCR_VALIDATED_ENTRY in bundle and
         BCR_RECORD_DIGEST in bundle,
         "promotion does not compare the proposed entry with the validated entry"),
        (BCR_FETCH_ENTRY in prepare and BCR_VERIFY_BUNDLE in recheck and
         BCR_VALIDATED_ENTRY in recheck and BCR_RECORD_DIGEST in recheck and
         all(binding in recheck and argument in recheck for binding, argument in BCR_PROMOTED),
         "fork preparation does not recheck the commit against the validated entry"),
    ])


def _ignored_failures(text):
    """Ways a job or script can turn a failed check into success."""
    found = ["continue-on-error"] if "continue-on-error" in text else []
    return found + ["||" for body in _run_bodies(text) if "||" in body][:1]


def _bcr_submission_violations(workflow):
    """BCR submission takes the record's digests and validated entry, and regenerates nothing."""
    jobs = _jobs(workflow)
    if set(jobs) != BCR_SUBMISSION_JOBS:
        return ["BCR submission jobs changed: %s" % sorted(jobs)]
    return _failed([
        (_triggers(workflow) == {"workflow_dispatch"},
         "BCR submission runs on an event other than a manual dispatch"),
        (_candidate_input_names(workflow) == list(BCR_SUBMISSION_INPUTS),
         "BCR submission inputs changed: %s" % _candidate_input_names(workflow)),
        (not _build_steps(workflow),
         "BCR submission builds or regenerates: %s" % _build_steps(workflow)),
        (not any("${{" in body for body in _run_bodies(workflow)),
         "BCR submission run blocks must use quoted environment values"),
        (not _ignored_failures(workflow),
         "BCR submission ignores a failure: %s" % _ignored_failures(workflow)),
    ]) + _bcr_validation_violations(jobs["validate"]) + _bcr_promotion_violations(
        jobs["promote-entry"], jobs["prepare-fork"])


DOCS_WORKFLOW = ".github/workflows/deploy_docs.yaml"
DOCS_DEPLOY_STEPS = ("Fetch the candidate record",
                     "Verify the candidate record attestation",
                     "Select the docs build to deploy",
                     "Download the retained Pages artifact",
                     "Verify selected source, artifact, and exact archive bytes",
                     "Deploy the selected artifact without rebuilding")
DOCS_SELECTED = "${{ steps.select.outputs."
# The exact guard line: a substring match would accept `inputs.selection == 'record' && false`.
DOCS_RECORD_ONLY = re.compile(r"(?m)^        if: inputs\.selection == 'record'$")
# The record the attestation step verifies is the one the selection step reads.
DOCS_RECORD_BINDINGS = ("          RECORD_PATH: ${{ steps.record.outputs.record_path }}\n",
                        "          RECORD_COMMIT: ${{ steps.record.outputs.record_commit }}\n")
# Steps that may read dispatch inputs: the record fetch, the attestation guard and the selection.
DOCS_INPUT_STEPS = DOCS_DEPLOY_STEPS[:3]


def _docs_record_violations(fetch, attest, select):
    return _failed([
        ("python3 -B -m tools.release_candidate docs-record" in fetch and
         '--record-run "$RECORD_RUN"' in fetch and '--record-sha256 "$RECORD_SHA256"' in fetch and
         DOCS_RECORD_ONLY.search(fetch) is not None,
         "deploy does not fetch the named candidate record"),
        ("gh attestation verify" in attest and
         "--signer-workflow jwmcglynn/donner/.github/workflows/release_candidate.yml" in attest and
         '--source-digest "$RECORD_COMMIT" --source-ref refs/heads/main' in attest and
         DOCS_RECORD_ONLY.search(attest) is not None and
         all(binding in attest + "\n" for binding in DOCS_RECORD_BINDINGS),
         "deploy does not verify the candidate record attestation"),
        ("python3 -B -m tools.release_candidate docs-selection" in select and
         '--selection "$SELECTION"' in select and '--record-path "$RECORD_PATH"' in select and
         '--record-sha256 "$RECORD_SHA256"' in select and
         all(binding in select for binding in DOCS_RECORD_BINDINGS),
         "deploy does not select from the candidate record"),
    ])


def _docs_selection_violations(deploy):
    fetch, attest, select, download, verify, publish = (_named_step(deploy, name)
                                                        for name in DOCS_DEPLOY_STEPS)
    input_steps = [_named_step(deploy, name) for name in DOCS_INPUT_STEPS]
    return _docs_record_violations(fetch, attest, select) + _failed([
        ("artifact-ids: %sartifact_id }}" % DOCS_SELECTED in download and
         "run-id: %srun_id }}" % DOCS_SELECTED in download,
         "deploy downloads an artifact the selection did not name"),
        ("python3 -B -m tools.pages_artifact verify" in verify and
         "DOC_ARCHIVE_SHA256: %sarchive_sha256 }}" % DOCS_SELECTED in verify,
         "deploy does not verify the selected archive"),
        ("DOC_ARTIFACT_ID: %sartifact_id }}" % DOCS_SELECTED in publish,
         "deploy promotes an artifact the selection did not name"),
        (all("inputs." not in step for step in _steps(deploy) if step not in input_steps),
         "deploy reads dispatch inputs outside the record and selection steps"),
    ])


def _docs_deploy_violations(workflow):
    """Docs deploy promotes exactly the selected retained build and builds nothing."""
    deploy = _jobs(workflow).get("deploy", "")
    names = [_step_identity_value(step, "name") for step in _steps(deploy)]
    guard = "if: github.event_name == 'workflow_dispatch' && github.ref == 'refs/heads/main'"
    return _failed([
        (guard in deploy, "deploy can run without a manual dispatch on main"),
        (not _build_steps(deploy) and "upload-artifact@" not in deploy,
         "deploy builds or uploads: %s" % _build_steps(deploy)),
        ([name for name in names if name in DOCS_DEPLOY_STEPS] == list(DOCS_DEPLOY_STEPS) and
         names[-1:] == [DOCS_DEPLOY_STEPS[-1]],
         "deploy does not select and verify before its final Pages step"),
        (not any("${{" in body for body in _run_bodies(deploy)),
         "deploy run blocks must use quoted environment values"),
        (not _ignored_failures(deploy), "deploy ignores a failure: %s" % _ignored_failures(deploy)),
    ]) + _docs_selection_violations(deploy)


class SecurityWorkflowPolicyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.codeql = _read(".github/workflows/codeql.yml")
        cls.fuzz = _read(".github/workflows/fuzz.yml")
        cls.sanitizers = _read(".github/workflows/sanitizers.yml")
        cls.supply_chain_files = _read_supply_chain_files()

    def test_artifact_collection_omits_nested_symlinks(self):
        action = self.supply_chain_files[
            ".github/actions/upload-bazel-test-artifacts/action.yml"
        ]
        scripts = _run_bodies(action)
        self.assertEqual(1, len(scripts))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            workspace = root / "workspace"
            workspace.mkdir()
            logs = root / "logs"
            target = logs / "package" / "test"
            outputs = target / "test.outputs"
            outputs.mkdir(parents=True)
            (target / "test.log").write_text("failure details", encoding="utf-8")
            (target / "test.xml").write_text("<testsuite/>", encoding="utf-8")
            (outputs / "actual.png").write_bytes(b"fixture image")
            outside = root / "outside"
            outside.mkdir()
            (outside / "test.log").write_text("outside sentinel", encoding="utf-8")
            (outputs / "linked.log").symlink_to(outside / "test.log")
            (outputs / "linked_directory").symlink_to(outside, target_is_directory=True)
            (logs / "linked_target").symlink_to(outside, target_is_directory=True)
            (workspace / "bazel-testlogs").symlink_to(logs, target_is_directory=True)
            runner = root / "runner"
            runner.mkdir()

            result = subprocess.run(
                ["bash", "-c", textwrap.dedent(scripts[0])],
                cwd=workspace,
                env={**os.environ, "RUNNER_TEMP": str(runner)},
                capture_output=True,
                text=True,
                timeout=test_timing.STALL_GUARD_SECONDS,
                check=False,
            )
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            artifacts = runner / "bazel-test-artifacts"
            collected = {
                path.relative_to(artifacts).as_posix(): path.read_bytes()
                for path in artifacts.rglob("*") if path.is_file()
            }
            self.assertEqual(
                {
                    "package/test/test.log": b"failure details",
                    "package/test/test.xml": b"<testsuite/>",
                    "package/test/test.outputs/actual.png": b"fixture image",
                },
                collected,
            )

    def test_failed_hosted_perf_retains_test_failure_artifacts(self):
        workflow = self.supply_chain_files[".github/workflows/perf.yml"]
        job = workflow.split("\n  perf:\n", 1)[1]
        self.assertIn("- os: xcode-27", job)
        self.assertIn("- os: ubuntu-24.04", job)
        self.assertIn("runs-on: ${{ matrix.os }}", job)
        test_step = _step_body(job, "Test perf-tagged targets")
        self.assertIn("id: perf", test_step)
        artifacts = _step_body(job, "Upload perf test failure artifacts")
        self.assertIn("if: failure() && steps.perf.outcome == 'failure'", artifacts)
        self.assertIn("uses: ./.github/actions/upload-bazel-test-artifacts", artifacts)
        for identity in ("github.run_id", "github.run_attempt", "github.job"):
            self.assertIn(identity, artifacts)

    def test_passing_perf_retains_test_logs_per_matrix_entry(self):
        # A passing browser responsiveness run prints more than Bazel's console limit, so its
        # measured values are only in its test log; keep the logs of passing runs too.
        workflow = self.supply_chain_files[".github/workflows/perf.yml"]
        job = workflow.split("\n  perf:\n", 1)[1]
        logs = _step_body(job, "Upload perf test logs")
        # A failing summary step after a passing test still keeps the log.
        self.assertIn("if: ${{ !cancelled() && steps.perf.outcome == 'success' }}", logs)
        self.assertIn("uses: ./.github/actions/upload-bazel-test-artifacts", logs)
        # Both matrix entries run the same job, so each artifact name needs the entry.
        for name in ("Upload perf test logs", "Upload perf test failure artifacts"):
            step = _step_body(job, name)
            for identity in ("github.run_id", "github.run_attempt", "matrix.target_tag"):
                self.assertIn(identity, step, name)

    def test_action_cache_preserves_setup_bazel_rc_without_a_final_newline(self):
        action = self.supply_chain_files[".github/actions/cache-bazel-actions/action.yml"]
        step = action.split("    - name: Enable disk action cache\n", 1)[1]
        body = step.split("      run:", 1)[1].split("\n\n", 1)[0]
        script = textwrap.dedent(body.split("\n", 1)[1]) if body.strip().startswith("|") else body.strip()
        with tempfile.TemporaryDirectory() as directory:
            rc = Path(directory) / ".bazelrc"
            original = "common --repository_cache=/fixture/repository"
            rc.write_text(original)
            result = subprocess.run(["/bin/bash", "-c", script], env={"HOME": directory},
                                    text=True, capture_output=True,
                                    timeout=test_timing.STALL_GUARD_SECONDS)
            self.assertEqual(result.returncode, 0, result.stderr)
            lines = rc.read_text().splitlines()
            self.assertEqual(lines[0], original)
            self.assertEqual(lines[1], "build --disk_cache=~/.cache/bazel-disk")

    def test_compiled_action_cache_refreshes_only_from_main(self):
        action = self.supply_chain_files[".github/actions/cache-bazel-actions/action.yml"]
        self.assertNotIn("uses: actions/cache@", action)
        reader = action.split("    - name: Restore main action cache\n", 1)[1].split("\n\n", 1)[0]
        writer = action.split("    - name: Save main action cache\n", 1)[1].split("\n\n", 1)[0]
        self.assertIn("if: inputs.mode == 'restore'\n", reader)
        self.assertIn("uses: actions/cache/restore@v6", reader)
        self.assertIn("uses: actions/cache/save@v6", writer)
        # Only a main push saves, and only a snapshot the bound step checked against its budget.
        condition = re.search(r"^      if: (.+)$", writer, re.MULTILINE).group(1)
        self.assertTrue(_main_write_only(condition), condition)
        for conjunct in ("inputs.mode == 'save'", "env.BAZEL_DISK_CACHE_EXACT_HIT != 'true'",
                         "steps.bound.outputs.save == 'true'"):
            self.assertIn(" && " + conjunct, condition)
        keys = [re.search(r"^        key: (.+)$", block, re.MULTILINE).group(1)
                for block in (reader, writer)]
        self.assertEqual(keys[0], keys[1])
        for dimension in ("runner.os", "runner.arch", "inputs.namespace", "github.sha"):
            self.assertIn(dimension, keys[0])
        self.assertIn("hashFiles('.bazelversion')", keys[0])
        restore = reader.split("restore-keys: |", 1)[1]
        self.assertNotIn("github.sha", restore)
        self.assertIn("inputs.namespace", restore)
        self.assertNotIn("restore-keys", writer)
        self.assertNotIn("secrets.", action)

    def _restoring_jobs(self):
        """(where, steps) for every workflow job that uses the action cache."""
        action_ref = "./.github/actions/cache-bazel-actions"
        for path, text in sorted(self.supply_chain_files.items()):
            if not path.startswith(".github/workflows/"):
                continue
            for name, job in _jobs(text).items():
                steps = _steps(job)
                if any(_step_action(step, 8) == action_ref for step in steps):
                    yield "%s:%s" % (path, name), steps

    def test_bounded_save_is_the_only_writer_of_action_cache_snapshots(self):
        # Every restore reads the newest snapshot under the key prefix, so the bound holds for
        # restores only while the bounded save is the one writer of that key series and the one
        # cache step on the disk cache in a job that restores it.
        action_path = ".github/actions/cache-bazel-actions/action.yml"
        for path, text in sorted(self.supply_chain_files.items()):
            if path != action_path:
                self.assertNotIn("bazel-actions-", text, path)
                self.assertNotIn(".cache/bazel-disk", text, path)
        # setup-bazel's own disk cache saves the same directory under its own keys.
        for where, steps in self._restoring_jobs():
            setups = [step for step in steps if _step_action(step, 8) == "bazel-contrib/setup-bazel"]
            self.assertEqual([], [step for step in setups if _with_key(step, "disk-cache", 8)],
                             where)
        action = self.supply_chain_files[action_path]
        savers = [step for step in _steps(action, indent=4)
                  if _step_action(step, 6) in ("actions/cache", "actions/cache/save")]
        self.assertEqual(1, len(savers))
        self.assertIn("steps.bound.outputs.save == 'true'", _step_key(savers[0], "if", 6))
        self.assertIn("key: bazel-actions-v2-", savers[0])

    def test_action_cache_save_is_the_last_step_of_every_restoring_job(self):
        # Eviction must see every Bazel command's cache use, and a job that restores without
        # saving would freeze its namespace's snapshot on main.
        action = "./.github/actions/cache-bazel-actions"
        restoring = 0
        for path, text in sorted(self.supply_chain_files.items()):
            if not path.startswith(".github/workflows/"):
                continue
            for name, job in _jobs(text).items():
                steps = [step for step in _steps(job) if _step_action(step, 8) == action]
                if not steps:
                    continue
                restoring += 1
                where = "%s:%s" % (path, name)
                self.assertEqual(2, len(steps), where)
                restore, save = steps
                self.assertIsNone(_with_key(restore, "mode", 8), where)
                self.assertEqual("save", _with_key(save, "mode", 8), where)
                self.assertIsNone(_step_key(save, "if", 8), where)
                self.assertEqual(_with_key(restore, "namespace", 8),
                                 _with_key(save, "namespace", 8), where)
                self.assertEqual(save, _steps(job)[-1], where)
                # tools/ci_runner_loss_rerun.py treats a step of this name as post-job cleanup.
                self.assertEqual("Save Bazel action cache", _step_key(save, "name", 8), where)
        self.assertEqual(4, restoring)

    def test_only_main_pushes_save_actions_caches(self):
        self.assertEqual([], _cache_write_violations(self.supply_chain_files))

    def test_cache_write_policy_rejects_pull_request_saves(self):
        files = self.supply_chain_files
        bcr = ".github/workflows/bcr_preflight.yml"
        cmake = ".github/workflows/cmake.yml"
        wasm = ".github/workflows/editor_wasm.yml"
        lint = ".github/workflows/lint.yml"
        action = ".github/actions/cache-bazel-actions/action.yml"
        main_save = "cache-save: ${{ %s }}" % MAIN_CACHE_WRITE
        save_if = "if: %s" % MAIN_CACHE_WRITE
        self.assertEqual(3, files[bcr].count(main_save))
        self._assert_rejected(lambda files, path: _cache_write_violations(files), {
            "setup-bazel default save": (bcr, files[bcr].replace(
                "          " + main_save + "\n", "", 1), "lets setup-bazel save outside main"),
            "setup-bazel saves on pull requests": (bcr, files[bcr].replace(
                main_save, "cache-save: ${{ github.event_name != 'schedule' }}", 1),
                "lets setup-bazel save outside main"),
            "setup-bazel always saves": (wasm, files[wasm].replace(
                main_save, "cache-save: true", 1), "lets setup-bazel save outside main"),
            "combined cache step": (cmake, files[cmake].replace(
                "uses: actions/cache/restore@v6", "uses: actions/cache@v6", 1),
                "saves an Actions cache outside main"),
            "ungated save": (lint, files[lint].replace(
                "        " + save_if + " && steps.bazel-cache.outputs.cache-hit != 'true'\n",
                "", 1), "saves an Actions cache outside main"),
            "widened save": (cmake, files[cmake].replace(
                save_if + " && steps.ccache", save_if + " || github.event_name == 'pull_request'"
                " && steps.ccache", 1), "saves an Actions cache outside main"),
            "branch push save": (cmake, files[cmake].replace(
                save_if + " && steps.ccache", "if: github.event_name != 'pull_request'"
                " && steps.ccache", 1), "saves an Actions cache outside main"),
            "composite action writer": (action, files[action].replace(
                "if: github.event_name != 'pull_request' && github.ref == 'refs/heads/main' && ",
                "if: github.ref == 'refs/heads/main' || github.event_name == 'push' && ", 1),
                "saves an Actions cache outside main"),
            "setup-node cache": (wasm, files[wasm].replace(
                "          package-manager-cache: false\n", "          cache: npm\n", 1),
                "saves on every event"),
            "setup-node automatic cache": (wasm, files[wasm].replace(
                "          package-manager-cache: false\n", "", 1), "saves on every event"),
            "split expressions": (lint, files[lint].replace(
                save_if + " && steps.bazel-cache.outputs.cache-hit != 'true'\n",
                "if: ${{ %s }} && ${{ steps.bazel-cache.outputs.cache-hit }}\n"
                % MAIN_CACHE_WRITE, 1), "saves an Actions cache outside main"),
            "literal cache-save string": (bcr, files[bcr].replace(
                main_save, "cache-save: " + MAIN_CACHE_WRITE, 1),
                "lets setup-bazel save outside main"),
            "quoted combined cache": (cmake, files[cmake].replace(
                "uses: actions/cache/restore@v6", 'uses: "Actions/Cache@v6"', 1),
                "saves an Actions cache outside main"),
            "unreviewed cache action": (cmake, files[cmake].replace(
                "uses: actions/cache/restore@v6", "uses: example/other-cache-action@v1", 1),
                "unreviewed cache behavior"),
            "self-caching action without cache in its name": (cmake, files[cmake].replace(
                "uses: actions/cache/restore@v6", "uses: example/setup-tool@v1", 1),
                "unreviewed cache behavior"),
            "cache-save outside the with block": (bcr, files[bcr].replace(
                "          " + main_save + "\n", "", 1).replace(
                "      - uses: bazel-contrib/setup-bazel@0.19.0\n",
                "      - uses: bazel-contrib/setup-bazel@0.19.0\n        env:\n"
                "          cache-save: false\n", 1), "lets setup-bazel save outside main"),
            "flow-style step": (lint, files[lint].replace(
                "      - name: Restore Bazel\n",
                "      - { uses: actions/cache@v6, with: { path: x, key: y } }\n"
                "      - name: Restore Bazel\n", 1), "step form the cache policy cannot read"),
            "aliased step": (lint, files[lint].replace(
                "      - name: Restore Bazel\n", "      - *save_step\n      - name: Restore Bazel\n",
                1), "step form the cache policy cannot read"),
            "setup-go default cache": (lint, files[lint].replace(
                "      - name: Restore Bazel\n",
                "      - uses: actions/setup-go@v6\n      - name: Restore Bazel\n", 1),
                "saves on every event"),
        })

    def test_self_hosted_macos_uses_buildbuddy_and_removes_its_key(self):
        workflow = self.supply_chain_files[".github/workflows/main.yml"]
        self_hosted = _jobs(workflow)["macos-self-hosted"]
        header = self_hosted.split("    steps:\n", 1)[0]
        self.assertNotIn("--remote_cache=", header)
        self.assertNotIn("BAZEL_REMOTE_CACHE", workflow)
        order = [self_hosted.index("      - name: %s\n" % name) for name in (
            "Select pinned Xcode", "Configure BuildBuddy remote cache",
            "Prepare self-hosted macOS toolchain")]
        self.assertEqual(order, sorted(order))
        cache = _step_body(self_hosted, "Configure BuildBuddy remote cache")
        self.assertIn("cache-partition: ${{ steps.xcode.outputs.cache_partition }}", cache)
        self.assertTrue(self_hosted.rstrip("\n").endswith(BUILDBUDDY_CLEANUP.rstrip("\n")))

    def test_cache_keys_never_reach_bazel_as_flags(self):
        # Bazel writes every rc option into its server log in the output base, which outlives
        # the job on a persistent runner, so a key may reach Bazel only through a credential
        # helper.
        for path, text in self.supply_chain_files.items():
            if path.startswith(".github/"):
                with self.subTest(path=path):
                    self.assertNotIn("--remote_header", text)
                    self.assertNotIn("x-buildbuddy-api-key=", text)

    def test_external_actions_use_released_version_tags(self):
        """External actions use Renovate-compatible release tags, never branches."""
        action_line = re.compile(r"^\s*(?:-\s*)?uses:\s*(?P<target>\S+)\s*$")
        released_version = re.compile(r"^v?\d+(?:\.\d+){0,2}$")
        pins = {}
        seen = 0

        for path, contents in self.supply_chain_files.items():
            for line_number, line in enumerate(contents.splitlines(), start=1):
                match = action_line.match(line)
                if not match or "@" not in match.group("target"):
                    continue

                target, revision = match.group("target").rsplit("@", 1)
                if target.startswith("./") or target.startswith("docker://"):
                    continue

                with self.subTest(path=path, line=line_number, action=target):
                    self.assertRegex(
                        revision,
                        released_version,
                        "%s:%d must use a released action version" % (path, line_number),
                    )

                pins.setdefault(target, set()).add(revision)
                seen += 1

        self.assertGreaterEqual(seen, 75, "external action discovery no longer covers the workflows")
        for target, revisions in pins.items():
            self.assertEqual(
                1,
                len(revisions),
                "%s is pinned inconsistently: %s" % (target, sorted(revisions)),
            )

    def test_downloaded_bazel_diff_is_verified_before_execution(self):
        """The downloaded JAR must match the digest published for its release."""
        workflow = self.supply_chain_files[".github/workflows/main.yml"]
        self.assertIn('BAZEL_DIFF_TAG: "%s"' % BAZEL_DIFF_TAG, workflow)
        self.assertIn('BAZEL_DIFF_SHA256: "%s"' % BAZEL_DIFF_SHA256, workflow)

        download = _step_body(workflow, "Download bazel-diff")
        verification = "sha256sum --check --status"
        self.assertIn("$BAZEL_DIFF_SHA256", download)
        self.assertIn(verification, download)
        self.assertLess(download.index(verification), download.index("exit 0"))
        self.assertIn('rm -f "$out"', download)

    def test_downloaded_tools_are_verified_before_extraction(self):
        badges = ".github/workflows/badges.yaml"
        docs_lane = _read("tools/ci/docs_lane.sh")
        self.assertEqual(_step_body(self.supply_chain_files[DOCS_WORKFLOW], "Install Doxygen").strip(),
                         "run: tools/ci/docs_lane.sh install-doxygen")
        self.assertIn('readonly DOXYGEN_SHA256="%s"\n' % DOXYGEN_SHA256, docs_lane)
        # //tools/ci:docs_lane_tests runs the script's exact download, check and extraction.
        cases = (
            (badges, _step_body(self.supply_chain_files[badges], "Install cloc"), CLOC_SHA256),
            ("tools/ci/docs_lane.sh", docs_lane.split("\ninstall_doxygen() {\n", 1)[1],
             '"${DOXYGEN_SHA256}  ${archive}"'),
        )
        for path, body, digest in cases:
            with self.subTest(path=path):
                self.assertIn(digest, body)
                verification = "sha256sum --check --strict"
                self.assertIn(verification, body)
                self.assertLess(body.index(verification), body.index("tar "))

    def test_documentation_publication_requires_a_manual_request(self):
        workflow = self.supply_chain_files[".github/workflows/deploy_docs.yaml"]
        deploy = workflow.split("\n  deploy:\n", 1)[1]
        self.assertIn("if: github.event_name == 'workflow_dispatch'", deploy)
        self.assertIn("branches: [\"main\"]", workflow)

    def test_docs_build_steps_run_their_tested_tools(self):
        # The docs build job keeps one command per step; //tools/ci:docs_lane_tests and
        # //tools:pages_artifact_tests pin what each command runs and writes.
        build = self.supply_chain_files[DOCS_WORKFLOW].split("\n  deploy:\n", 1)[0]
        steps = {
            "Install graphviz": "uses: ./.github/actions/apt-install\n        with:\n"
                                "          packages: graphviz\n"
                                # A plain apt-get install keeps Graphviz's recommended fonts.
                                "          install-recommends: \"true\"",
            "Install Doxygen": "run: tools/ci/docs_lane.sh install-doxygen",
            "Generate Doxygen documentation": "run: tools/build_docs.sh",
            "Bind the docs site to this source run": "run: python3 -B -m tools.pages_artifact "
                                                     "marker --output generated-doxygen/html/"
                                                     "docs-build.json",
            "Record the immutable docs selection": "env:\n          ARTIFACT_ID: "
                                                   "${{ steps.pages.outputs.artifact_id }}\n"
                                                   "        run: python3 -B -m "
                                                   "tools.pages_artifact summary --archive "
                                                   "\"$RUNNER_TEMP/artifact.tar\"",
            "Record the docs build dependency state": "run: tools/ci/docs_lane.sh "
                                                      "record-dependencies",
        }
        for name, body in steps.items():
            with self.subTest(step=name):
                self.assertEqual(_step_body(build, name).split("\n\n", 1)[0].strip(), body)

    def test_release_candidate_artifacts_share_the_maximum_retention(self):
        # tools/release_candidate.py requires each candidate artifact to stay retained through the
        # release and rollback window. Every role's upload keeps the 90-day maximum, so a
        # candidate is verifiable for the same 30 days after its builds whichever role is oldest.
        uploads = (
            (".github/workflows/bcr_preflight.yml", "upload-artifact@", "name: donner-bcr-qualified-"),
            (".github/workflows/bcr_preflight.yml", "upload-artifact@",
             "name: donner-svg-linux-x86-64-"),
            (".github/workflows/bcr_preflight.yml", "upload-artifact@",
             "name: donner-svg-darwin-arm64-"),
            (".github/workflows/editor_wasm.yml", "upload-artifact@",
             "name: donner-editor-wasm-geode-"),
            (".github/workflows/deploy_docs.yaml", "upload-pages-artifact@", "retention-days:"),
            (".github/workflows/deploy_docs.yaml", "upload-artifact@",
             "name: donner-docs-provenance-"),
            (CANDIDATE_WORKFLOW, "upload-artifact@", "name: donner-release-candidate-"),
        )
        for path, action, name in uploads:
            with self.subTest(path=path, name=name):
                steps = [step for step in self.supply_chain_files[path].split("\n      - ")
                         if action in step and name in step]
                self.assertEqual(len(steps), 1)
                self.assertRegex(steps[0], r"\n          retention-days: 90\n")

    def test_release_publishes_only_the_verified_candidate_record(self):
        release = self.supply_chain_files[".github/workflows/release.yml"]
        self.assertEqual([], _release_publication_violations(release))
        self.assertIn("permissions:\n  contents: read", release)
        self.assertIn("RELEASE_UPLOAD_URL: ${{ github.event.release.upload_url }}", release)
        self.assertNotIn("softprops/action-gh-release", release)
        self.assertNotIn("publish-to-bcr", release)
        self.assertNotIn("svenstaro/upload-release-action", release)

    def test_release_policy_rejects_builds_preflight_selection_and_extra_assets(self):
        path = ".github/workflows/release.yml"
        release = self.supply_chain_files[path]
        stage = "          python3 -B -m tools.release_candidate release --source-root . \\\n"
        upload = '            "$RUNNER_TEMP"/release/donner-*.candidate.json\n'
        self._assert_rejected(lambda files, path: _release_publication_violations(files[path]), {
            "build": (path, release.replace(
                stage, "          bazelisk build -c opt //donner/svg/tool:donner-svg\n" + stage, 1),
                "release builds"),
            "doxygen": (path, release.replace(stage, "          tools/build_docs.sh\n" + stage, 1),
                        "release builds"),
            "no record verification": (path, release.replace(
                stage, "          python3 -B -m tools.release_candidate verify \\\n", 1),
                "re-verify the named record"),
            "preflight line": (path, release.replace(
                "tools.bcr_release select-record", "tools.bcr_release select-preflight", 1),
                "select-preflight"),
            "extra asset": (path, release.replace(
                upload, upload[:-1] + ' \\\n            "$RUNNER_TEMP"/candidate-work/artifacts/editor.zip\n', 1),
                "release uploads files"),
            "retained upload": (path, release + "      - uses: actions/upload-artifact@v7\n",
                                "actions/upload-artifact@"),
            "no record attestation": (path, release.replace(
                "release_candidate.yml", "bcr_preflight.yml", 1), "attestations"),
            "unpinned record ref": (path, release.replace(
                " --source-ref refs/heads/main", "", 1), "attestations"),
            "upload before verification": (path, release.replace(
                "      - name: Verify preflight build attestations\n",
                "      - name: Check preflight build attestations\n", 1), "before its final upload"),
            "unchecked release source": (path, release.replace(
                "          " + RELEASE_SOURCE_CHECKS[0] + "\n", "", 1), "release commit"),
            "release source check removed": (path, release.replace(
                "      - name: %s\n%s" % (RELEASE_SOURCE_STEP,
                                         _step_body(release, RELEASE_SOURCE_STEP)), "", 1),
                "release commit"),
            "unverified staged CLI": (path, release.replace(
                "tools.release_cli verify-release", "tools.release_cli check-lockfile", 1),
                "staged CLI files"),
            "action before the source check": (path, release.replace(
                "      - name: " + RELEASE_SOURCE_STEP + "\n",
                "      - uses: ./.github/actions/setup\n\n      - name: " + RELEASE_SOURCE_STEP +
                "\n", 1), "release commit"),
            "staged CLI of another run": (path, release.replace(
                '--run-id "$SOURCE_RUN_ID" --attempt "$SOURCE_ATTEMPT"\n\n'
                "      - name: Verify preflight",
                '--run-id "$RECORD_RUN_ID" --attempt "$SOURCE_ATTEMPT"\n\n'
                "      - name: Verify preflight", 1), "staged CLI files"),
            "staged CLI of another commit": (path, release.replace(
                '--release "$RUNNER_TEMP/release" \\\n            --commit "$GITHUB_SHA"',
                '--release "$RUNNER_TEMP/release" \\\n            --commit "$SOURCE_COMMIT"', 1),
                "staged CLI files"),
        })

    def test_candidate_assembly_is_manual_minimal_and_retained(self):
        workflow = self.supply_chain_files[CANDIDATE_WORKFLOW]
        self.assertEqual([], _candidate_assembly_violations(workflow))

    def test_candidate_assembly_policy_rejects_widened_or_building_workflows(self):
        path = CANDIDATE_WORKFLOW
        workflow = self.supply_chain_files[path]
        dispatch = "on:\n  workflow_dispatch:\n"
        assemble_scopes = "      actions: read\n      issues: read\n"
        step = "          python3 -B -m tools.release_candidate assemble \\\n"
        self._assert_rejected(lambda files, path: _candidate_assembly_violations(files[path]), {
            "push trigger": (path, workflow.replace(
                dispatch, "on:\n  push:\n    branches: [main]\n  workflow_dispatch:\n", 1),
                "manual dispatch"),
            "write scope": (path, workflow.replace(
                assemble_scopes, assemble_scopes + "      contents: write\n", 1), "token scopes"),
            "workflow write": (path, workflow.replace(
                "permissions:\n  contents: read\n", "permissions:\n  contents: write\n", 1),
                "widens"),
            "secret": (path, workflow.replace(
                "      GH_TOKEN: ${{ github.token }}\n",
                "      GH_TOKEN: ${{ secrets.PUBLISH_TOKEN }}\n", 1), "secret"),
            "build": (path, workflow.replace(
                step, "          bazelisk build //tools/ci:editor_wasm_package\n" + step, 1),
                "assembly builds"),
            "commit pin": (path, workflow.replace(
                "actions/upload-artifact@v7", "actions/upload-artifact@" + "0" * 40, 1),
                "pins an action"),
            "short retention": (path, workflow.replace(
                "          retention-days: 90\n", "          retention-days: 30\n", 1),
                "90 days"),
            "input in script": (path, workflow.replace(
                '--commit "$CANDIDATE_COMMIT"', "--commit ${{ inputs.candidate_commit }}", 1),
                "interpolates"),
            "publishes": (path, workflow.replace(
                step, "          gh release upload v0.8.0 candidate.json\n" + step, 1),
                "assembly publishes"),
            "unchecked attestation": (path, workflow.replace(
                "sha256sum --check --strict candidate.json.sha256", "true", 1),
                "sign the checked record"),
            "optional input": (path, workflow.replace(
                "        required: true\n", "        required: false\n", 1), "inputs changed"),
        })

    def test_published_artifact_builds_record_their_module_graph(self):
        # Each artifact a release candidate binds records the generated MODULE.bazel.lock its
        # build resolved and proved in error mode, never a committed lockfile.
        editor = self.supply_chain_files[".github/workflows/editor_wasm.yml"]
        self.assertIn('        run: tools/ci/editor_wasm_lane.sh prove-lockfile '
                      '"$RUNNER_TEMP/editor-wasm-module.lock"\n',
                      _step_body(editor, "Prove the package's resolved module graph"))
        lane = _read("tools/ci/editor_wasm_lane.sh")
        prove = re.sub(r"\\\n\s*", "", lane.split("\nprove_lockfile() {\n", 1)[1].split(
            "\n}\n", 1)[0])
        update = "bazelisk build --config=editor-wasm --lockfile_mode=update //tools/ci:editor_wasm_package"
        error = "bazelisk build --config=editor-wasm --lockfile_mode=error //tools/ci:editor_wasm_package"
        check = "python3 -B -m tools.release_cli check-lockfile --lockfile MODULE.bazel.lock"
        snapshot = 'cp MODULE.bazel.lock "${snapshot}"'
        self.assertIn('local snapshot="$1"', prove)
        self.assertLess(prove.index(update), prove.index(error))
        self.assertLess(prove.index(error), prove.index(check))
        self.assertLess(prove.index(check), prove.index(snapshot))
        self.assertLess(editor.index("- name: Prove the package's resolved module graph"),
                        editor.index("- name: Stage package for handoff"))
        self.assertIn("        run: python3 %s candidate\n" % EDITOR_WASM_STAGE,
                      _step_body(editor, "Stage immutable Geode deployment candidate"))
        stage = _read(EDITOR_WASM_STAGE)
        self.assertIn('_copy_file(Path(f"{runner_temp}/editor-wasm-module.lock"), module_lock)',
                      stage)
        self.assertIn('module_lock = Path(candidate_dir) / "MODULE.bazel.lock"', stage)
        self.assertIn("generated_lock_sha256 = _sha256(module_lock)", stage)
        self.assertIn('"generated_lock_sha256":"%s"', stage)

        docs = self.supply_chain_files[".github/workflows/deploy_docs.yaml"]
        build = docs.split("\n  deploy:\n", 1)[0]
        self.assertEqual(_step_body(build, "Record the docs build dependency state").strip(),
                         "run: tools/ci/docs_lane.sh record-dependencies")
        # //tools/ci:docs_lane_tests runs these phases and pins their exact commands.
        record = _read("tools/ci/docs_lane.sh").split("record_dependencies() {", 1)[1]
        self.assertIn("bazelisk mod graph --ignore_dev_dependency \"--lockfile_mode=$1\"",
                      _read("tools/ci/docs_lane.sh"))
        update = "mod_graph update"
        error = "mod_graph error"
        self.assertLess(record.index(update), record.index(error))
        self.assertLess(record.index(error), record.index("python3 -B -m tools.pages_artifact provenance"))
        self.assertLess(build.index("- name: Upload retained Pages artifact"),
                        build.index("- name: Record the docs build dependency state"))
        retain = _step_body(build, "Retain the docs build provenance")
        self.assertIn("name: donner-docs-provenance-${{ github.run_attempt }}", retain)
        self.assertIn("path: ${{ runner.temp }}/docs-provenance/", retain)
        deploy = docs.split("\n  deploy:\n", 1)[1]
        self.assertEqual([], _build_steps(deploy.replace("bazel-contrib", "")))

    def test_bcr_preflight_has_no_publication_credentials(self):
        preflight = self.supply_chain_files[".github/workflows/bcr_preflight.yml"]
        self.assertIn("workflow_dispatch:", preflight)
        self.assertNotIn("secrets.", preflight)
        self.assertNotIn("contents: write", preflight)
        self.assertIn("tools.bcr_admission", preflight)
        self.assertIn("needs: [prepare, consumer, cli-linux, cli-macos]", preflight)
        self.assertEqual(preflight.count("tools.release_cli package"), 2)
        self.assertEqual(preflight.count("tools.release_cli verify"), 4)
        # Each CLI ships the NOTICE built with it, after the closure coverage test passes. The
        # compiler identity probe is built in the same invocations, so its configuration (and
        # therefore its compiler) is the CLI's.
        for mode in ("update", "error"):
            self.assertEqual(preflight.count(
                f"bazelisk build --lockfile_mode={mode} -c opt "
                "//tools/ci:release_svg //tools/ci:release_svg_notice \\\n"
                "            //tools/ci:release_compiler_identity\n"), 2)
        self.assertEqual(preflight.count(
            "--compiler-probe bazel-bin/tools/ci/release_compiler_identity \\\n"), 2)
        self.assertNotIn("c++ --version", preflight)
        self.assertEqual(preflight.count(
            "bazelisk test --lockfile_mode=error -c opt //tools/ci:release_svg_notice_coverage\n"), 2)
        self.assertEqual(preflight.count(
            "--notice bazel-bin/third_party/licenses/notice_cli.txt \\\n"), 2)
        for job_name in ("cli-linux", "cli-macos"):
            job = preflight.split("\n  %s:\n" % job_name, 1)[1].split("\n  cli-macos:\n", 1)[0]
            self.assertLess(job.index("//tools/ci:release_svg_notice_coverage"),
                            job.index("tools.release_cli package"))
        for platform in ("linux_x86_64", "darwin_arm64"):
            self.assertIn(f"/donner-svg_{platform}.NOTICE.txt\n", preflight)
        self.assertIn("actions/attest-build-provenance@", preflight)
        self.assertIn("if: github.ref == 'refs/heads/main'", preflight)
        self.assertIn("id-token: write", preflight)
        self.assertIn("attestations: write", preflight)
        self.assertIn("retention-days: 90", preflight)
        self.assertIn("donner-bcr-qualified-${{ github.run_attempt }}", preflight)
        self.assertIn("tools.bcr_source qualify", preflight)

    def test_bcr_cli_artifacts_stay_bound_to_one_attempt_with_a_rerun_explanation(self):
        # The CLI artifacts are bound to the attempt that built them, which the release path
        # pins. A rerun of only failed jobs that carries a passed CLI build over cannot find
        # them, so each consumer explains that all jobs must be rerun.
        preflight = self.supply_chain_files[".github/workflows/bcr_preflight.yml"]
        for job_name in ("attest-artifacts", "qualify"):
            job = preflight.split("\n  %s:\n" % job_name, 1)[1].split("\n  qualify:\n", 1)[0]
            downloads = []
            for step_id, platform in (("cli-linux-artifact", "linux-x86-64"),
                                      ("cli-macos-artifact", "darwin-arm64")):
                download = (
                    "      - id: %s\n"
                    "        uses: actions/download-artifact@v8\n"
                    "        with:\n"
                    "          name: donner-svg-%s-${{ needs.prepare.outputs.source_commit }}"
                    "-${{ github.run_attempt }}\n" % (step_id, platform)
                )
                self.assertEqual(job.count(download), 1, "%s %s" % (job_name, step_id))
                downloads.append(job.index(download))
            explain_at = job.index("      - name: Explain missing CLI artifacts\n")
            self.assertLess(max(downloads), explain_at, job_name)
            explain = _step_body(job, "Explain missing CLI artifacts")
            self.assertIn(
                "if: failure() && (steps.cli-linux-artifact.outcome == 'failure'"
                " || steps.cli-macos-artifact.outcome == 'failure')",
                explain,
                job_name,
            )
            # A download can also fail for transient reasons, so the explanation is conditional,
            # and the attempt reaches the script through the environment, not an expression.
            self.assertIn("RUN_ATTEMPT: ${{ github.run_attempt }}", explain, job_name)
            script = explain.split("        run: |\n", 1)[1]
            self.assertNotIn("${{", script, job_name)
            self.assertIn('if [ "$RUN_ATTEMPT" -gt 1 ]; then', script, job_name)
            for message in (
                "If the download error above is 'Artifact not found', this attempt reran only"
                " failed jobs, and the CLI artifacts are bound to the attempt that built them;"
                " rerun all jobs.",
                "Otherwise the download failed for another reason; see the error above and the"
                " GitHub status page before rerunning.",
            ):
                self.assertIn(message, script, job_name)
            self.assertLess(
                script.index("rerun all jobs."), script.index("Otherwise the download"), job_name)
            self.assertIn("exit 1", script, job_name)

    def _bcr_consumer_audit(self, preflight):
        """The consumer job, its audit step, the step's run body, query and audit call."""
        consumer = preflight.split("\n  consumer:\n", 1)[1].split("\n  cli-linux:\n", 1)[0]
        step = _step_body(consumer, "Exercise the archive through registry resolution")
        run = step.split("        run: |\n", 1)[1]
        audit = "tools/bcr_dependency_closure.py"
        query = run.split("bazelisk cquery", 1)[1].split(audit, 1)[0]
        return consumer, step, run, query, run.split(audit, 1)[1]

    def _assert_bcr_consumer_audit(self, preflight):
        audit = "tools/bcr_dependency_closure.py"
        self.assertEqual(preflight.count(audit), 1)
        consumer, step, run, query, audit_call = self._bcr_consumer_audit(preflight)
        self.assertEqual(run.count(audit), 1)
        self.assertNotIn("continue-on-error", consumer)
        self.assertNotRegex(step, r"(?m)^        (?:if|continue-on-error|shell):")
        self.assertNotIn("|", run, "a pipe or || would hide the query or audit status")
        self.assertNotIn("set +e", run)
        for library in ("base:base", "css:css", "svg:svg", "svg/renderer:renderer"):
            self.assertIn("@donner//donner/%s" % library, query)
        self.assertIn("--output=label_kind", query)
        for narrowing in ("--noimplicit_deps", "--notool_deps", "--implicit_deps=",
                          "--tool_deps=", "--keep_going"):
            self.assertNotIn(narrowing, run)
        # The audit is the step's last command, so its status is the step's status.
        audit_command = run[run.rindex("python3 -B", 0, run.index(audit)):].rstrip()
        joined = re.sub(r"\\\n\s*", " ", audit_command)
        self.assertNotIn("\n", joined, "a command after the audit would replace its status")
        self.assertNotRegex(joined, r";|&&")
        redirect = re.findall(r'> ("[^"]+")', query)
        labels = re.findall(r'--labels ("[^"]+")', audit_call)
        self.assertEqual(len(redirect), 1)
        self.assertEqual(redirect, labels, "the audit must read the file the query wrote")
        self.assertLess(run.index("bazelisk build"), run.index("bazelisk cquery"))
        self.assertIn("matrix:", consumer)
        self.assertIn("os: [ubuntu-24.04, xcode-27]", consumer)
        self.assertIn("bazel: ['7.x', '8.x']", consumer)

    def _bcr_scope(self, changed=(), event_name="pull_request", base=None, renamed=None):
        """Run the preflight scope step on a fixture repository; returns `full_matrix`."""
        preflight = self.supply_chain_files[".github/workflows/bcr_preflight.yml"]
        step = _step_body(_jobs(preflight)["prepare"], "Select the preflight scope")
        script = textwrap.dedent(step.split("        run: |\n", 1)[1])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo = root / "repo"
            repo.mkdir()
            env = {"PATH": os.environ.get("PATH", os.defpath), "HOME": directory,
                   "GIT_CONFIG_NOSYSTEM": "1", "GIT_AUTHOR_NAME": "fixture",
                   "GIT_AUTHOR_EMAIL": "fixture@example.invalid",
                   "GIT_COMMITTER_NAME": "fixture",
                   "GIT_COMMITTER_EMAIL": "fixture@example.invalid"}

            def git(*args):
                return subprocess.run(["git", *args], cwd=repo, env=env, check=True,
                                      text=True, capture_output=True).stdout.strip()

            def write(path, text):
                (repo / path).parent.mkdir(parents=True, exist_ok=True)
                (repo / path).write_text(text, encoding="utf-8")
                git("add", path)

            git("init", "-q")
            write("README.md", "base\n")
            if renamed:
                write(renamed[0], "a file long enough to be detected as a rename\n" * 8)
            git("commit", "-q", "-m", "base")
            base_sha = git("rev-parse", "HEAD")
            if renamed:
                (repo / renamed[1]).parent.mkdir(parents=True, exist_ok=True)
                git("mv", *renamed)
            for path in changed:
                write(path, "change\n")
            git("commit", "-q", "--allow-empty", "-m", "head")
            output = root / "output"
            result = subprocess.run(
                ["/bin/bash", "-c", script], cwd=repo, text=True, capture_output=True,
                env=dict(env, EVENT_NAME=event_name, BASE_SHA=base or base_sha,
                         HEAD_SHA=git("rev-parse", "HEAD"), GITHUB_OUTPUT=str(output)),
                timeout=test_timing.STALL_GUARD_SECONDS)
            self.assertEqual(0, result.returncode, result.stderr)
            values = dict(line.split("=", 1) for line in output.read_text().splitlines())
            return values["full_matrix"]

    def test_bcr_ordinary_pull_requests_run_the_slim_check(self):
        for changed in (["donner/svg/SVGElement.cc"], ["donner/base/Utils.h", "docs/notes.md"],
                        ["donner/svg/BUILD.bazel"], ["build_defs/rules.bzl"],
                        ["third_party/resvg-test-suite/fixture.svg"], ["tools/lint.sh"],
                        ["tools/ci/test_with_exclusions.py"], [".github/workflows/main.yml"]):
            with self.subTest(changed=changed):
                self.assertEqual("false", self._bcr_scope(changed))

    def test_bcr_release_path_pull_requests_run_the_full_matrix(self):
        for changed in (["MODULE.bazel"], ["MODULE.bazel.lock"], [".bazelversion"],
                        [".bazelrc"], [".bcr/presubmit.yml"], ["examples/bazel_consumer/main.cc"],
                        ["third_party/licenses/BUILD.bazel"], ["tools/bcr_source.py"],
                        ["tools/bcr_dependency_closure.py"], ["tools/release_cli.py"],
                        ["tools/notice_coverage_check.py"], ["tools/render_notice.py"],
                        ["tools/ci/BUILD.bazel"], ["tools/ci/release_compiler_identity.cc"],
                        ["tools/BUILD.bazel"], ["tools/default_notice_freshness_tests.py"],
                        ["build_defs/licenses.bzl"], ["build_defs/dep_audit.bzl"],
                        ["third_party/bazel/non_bcr_deps.bzl"], ["third_party/BUILD.harfbuzz"],
                        ["third_party/cmark-gfm.BUILD"], ["third_party/stb.bzl"],
                        ["third_party/patches/fixture.patch"],
                        [".github/workflows/bcr_preflight.yml"],
                        [".github/workflows/publish_bcr.yml"],
                        [".github/workflows/release.yml"],
                        ["donner/svg/SVGElement.cc", "MODULE.bazel"]):
            with self.subTest(changed=changed):
                self.assertEqual("true", self._bcr_scope(changed))
        # A rename out of a release path is a release-path change.
        moved = ("third_party/licenses/fixture.LICENSE.txt", "donner/fixture.LICENSE.txt")
        self.assertEqual("true", self._bcr_scope(renamed=moved))

    def test_bcr_scope_keeps_main_slim_and_qualification_full(self):
        # Pushes to main always run the slim check, whatever they change; the full matrix
        # qualifies candidates on schedule and dispatch. A diff that cannot be read is full.
        self.assertEqual("false", self._bcr_scope(["MODULE.bazel"], event_name="push"))
        self.assertEqual("true", self._bcr_scope(event_name="schedule"))
        self.assertEqual("true", self._bcr_scope(event_name="workflow_dispatch"))
        self.assertEqual("true", self._bcr_scope(["donner/a.cc"], base="0" * 40))
        self.assertEqual("true", self._bcr_scope([]))
        preflight = self.supply_chain_files[".github/workflows/bcr_preflight.yml"]
        self.assertTrue({"push", "pull_request", "schedule", "workflow_dispatch"} <=
                        _triggers(preflight))
        jobs = _jobs(preflight)
        full = "needs.prepare.outputs.full_matrix == 'true'"
        self.assertIn("full_matrix: ${{ steps.scope.outputs.full_matrix }}", jobs["prepare"])
        # The slim check always runs: prepare and the Ubuntu consumer on the newest Bazel.
        self.assertEqual("", _job_key(jobs["prepare"], "if"))
        self.assertEqual("", _job_key(jobs["consumer"], "if"))
        self.assertIn("        os: [ubuntu-24.04, xcode-27]\n", jobs["consumer"])
        self.assertIn("        bazel: ['7.x', '8.x']\n", jobs["consumer"])
        exclusions = _indented_block(
            jobs["consumer"].split("        exclude:\n", 1)[1].splitlines(), 8)
        self.assertEqual(["          - os: ${{ %s && 'none' || 'xcode-27' }}" % full,
                          "          - bazel: ${{ %s && 'none' || '7.x' }}" % full],
                         [line for line in exclusions if line.strip() and not _is_comment(line)])
        for job in ("cli-linux", "cli-macos", "qualify"):
            self.assertEqual("${{ %s }}" % full, _job_key(jobs[job], "if"), job)
        self.assertEqual("github.ref == 'refs/heads/main' && " + full,
                         _job_key(jobs["attest-artifacts"], "if"))
        self.assertEqual("[prepare, consumer, cli-linux, cli-macos]",
                         _job_key(jobs["qualify"], "needs"))

    def test_bcr_consumers_audit_the_configured_closure_for_rust(self):
        preflight = self.supply_chain_files[".github/workflows/bcr_preflight.yml"]
        self._assert_bcr_consumer_audit(preflight)

    def test_bcr_consumer_audit_contract_rejects_weakened_steps(self):
        preflight = self.supply_chain_files[".github/workflows/bcr_preflight.yml"]
        step = "      - name: Exercise the archive through registry resolution\n"
        audit_call = '"$GITHUB_WORKSPACE/tools/bcr_dependency_closure.py"'
        query = "--output=label_kind"
        _, _, run, _, _ = self._bcr_consumer_audit(preflight)
        redirect = re.findall(r'> ("[^"]+")', run)[0]
        weakened = {
            "continue-on-error": preflight.replace(
                step, step + "        continue-on-error: true\n"),
            "if": preflight.replace(step, step + "        if: always()\n"),
            "or true": preflight.replace(audit_call, audit_call + " || true", 1),
            "no implicit deps": preflight.replace(query, query + " --noimplicit_deps", 1),
            "no tool deps": preflight.replace(query, query + " --notool_deps", 1),
            "keep going": preflight.replace(query, query + " --keep_going", 1),
            "implicit deps false": preflight.replace(query, query + " --implicit_deps=false", 1),
            "tool deps false": preflight.replace(query, query + " --tool_deps=false", 1),
            "shell": preflight.replace(step, step + "        shell: bash {0}\n"),
            "set +e": preflight.replace(
                "          test \"$(git -C", "          set +e\n          test \"$(git -C", 1),
            "semicolon true": preflight.replace(
                '--labels "$RUNNER_TEMP/donner-bcr/consumer-closure.labels"',
                '--labels "$RUNNER_TEMP/donner-bcr/consumer-closure.labels"; true', 1),
            "command after the audit": preflight.replace(
                '--labels "$RUNNER_TEMP/donner-bcr/consumer-closure.labels"\n',
                '--labels "$RUNNER_TEMP/donner-bcr/consumer-closure.labels"\n          true\n', 1),
            "other labels file": preflight.replace(
                "--labels " + redirect, '--labels "$RUNNER_TEMP/other.labels"', 1),
        }
        for name, workflow in weakened.items():
            with self.subTest(name=name):
                self.assertNotEqual(workflow, preflight)
                with self.assertRaises(AssertionError):
                    self._assert_bcr_consumer_audit(workflow)

    def test_bcr_fork_preparation_requires_manual_approval_and_never_files_a_pr(self):
        workflow = self.supply_chain_files[".github/workflows/publish_bcr.yml"]
        self.assertIn("workflow_dispatch:", workflow)
        self.assertNotIn("workflow_run:", workflow)
        self.assertEqual([], _bcr_submission_violations(workflow))
        self.assertIn("github.ref == 'refs/heads/main'", workflow)
        self.assertIn("ref: ${{ github.sha }}", workflow)
        validate, remainder = workflow.split("\n  promote-entry:\n", 1)
        generate, remainder = remainder.split("\n  prepare-fork:\n", 1)
        prepare, publish = remainder.split("\n  publish-fork:\n", 1)
        self.assertNotIn("secrets.", validate)
        self.assertNotIn("secrets.", generate)
        # Submission promotes the entry preflight validated; it never runs the generator.
        self.assertNotIn("publish-to-bcr", workflow)
        self.assertIn("actions/upload-artifact@v7", generate)
        self.assertIn("actions/download-artifact@v8", prepare)
        self.assertEqual(workflow.count("actions/checkout@v7"), 7)
        self.assertNotRegex(workflow, r"uses: [^\n]+@[0-9a-f]{40}(?:\s|$)")
        self.assertIn("tools.bcr_release bundle-entry", generate)
        self.assertIn('--bundle "$GITHUB_WORKSPACE/bcr-entry.bundle"', generate)
        self.assertIn("path: bcr-entry.bundle", generate)
        self.assertIn("actions/upload-artifact@", generate)
        self.assertIn("fetch-depth: 0", _step_body(generate, "Checkout BCR for the proposed entry"))
        self.assertIn("needs.validate.outputs.prepare == 'true'", prepare)
        self.assertIn("needs: [validate, promote-entry]", prepare)
        self.assertNotIn("secrets.", prepare)
        self.assertIn("tools.bcr_release verify-bundle", prepare)
        self.assertIn('--bundle "$GITHUB_WORKSPACE/prepared/bcr-entry.bundle"', prepare)
        self.assertIn("actions/download-artifact@", prepare)
        self.assertIn("fetch-depth: 0", _step_body(prepare, "Checkout the exact BCR base"))
        self.assertIn("needs: [validate, promote-entry, prepare-fork]", publish)
        self.assertIn("environment: bcr-fork-preparation", publish)
        self.assertIn("PUBLISH_TOKEN: ${{ secrets.PUBLISH_TOKEN }}", publish)
        self.assertNotIn("uses:", publish, "the protected token job must run no external action")
        download = _step_body(publish, "Download and reverify the entry without a fork token")
        self.assertNotIn("PUBLISH_TOKEN", download)
        self.assertIn("gh run download", download)
        self.assertIn("sha256sum -c -", publish)
        self.assertIn("bundle verify", publish)
        self.assertIn("rev-parse 'FETCH_HEAD^{tree}'", publish)
        self.assertIn("ls-remote --exit-code", publish)
        self.assertIn("--force-with-lease=\"refs/heads/donner-${TAG}:\"", publish)
        self.assertNotIn("git push --force", publish)
        self.assertNotIn("/pulls", publish)
        self.assertNotIn("secrets.BCR_FORK_PUSH_TOKEN", publish)
        self.assertNotIn("secrets.BCR_PUBLISH_TOKEN", workflow)
        self.assertIn("cancel-in-progress: false", workflow)
        for path in (".github/workflows/bcr_preflight.yml", ".github/workflows/publish_bcr.yml"):
            for body in _run_bodies(self.supply_chain_files[path]):
                self.assertNotIn("${{", body, "run blocks must quote environment values")

    def test_bcr_submission_policy_rejects_regenerated_or_unbound_entries(self):
        path = BCR_SUBMISSION_WORKFLOW
        workflow = self.supply_chain_files[path]
        stage = "      - name: Stage the validated entry with derived metadata\n"
        generator = ("      - name: Generate the registry entry\n"
                     "        uses: bazel-contrib/publish-to-bcr@v1.5.1\n"
                     "        with:\n          local-registry: registry\n")
        approved = ("      approved_source_sha256:\n        description: Reviewed digest\n"
                    "        required: true\n        type: string\n")
        self._assert_rejected(lambda files, path: _bcr_submission_violations(files[path]), {
            "regenerated entry": (path, workflow.replace(stage, generator + stage, 1),
                                  "regenerates"),
            "free-form digest": (path, workflow.replace(
                "      record_sha256:\n", approved + "      record_sha256:\n", 1),
                "inputs changed"),
            "no record in plan": (path, workflow.replace(
                '            --record-sha256 "$RECORD_SHA256" \\\n            --work "$RUNNER_TEMP/bcr-plan"',
                '            --work "$RUNNER_TEMP/bcr-plan"', 1),
                "planned from the approved candidate record"),
            "no attestation scope": (path, workflow.replace(
                "      attestations: read\n", "", 1), "validation job token scopes"),
            "unfetched entry": (path, workflow.replace(
                "python3 -B -m tools.bcr_release fetch-entry", "true", 1),
                "submit the admission-validated entry"),
            "unstaged entry": (path, workflow.replace(
                "tools.bcr_release stage-entry", "tools.bcr_release verify-generated", 1),
                "submit the admission-validated entry"),
            "uncompared bundle": (path, workflow.replace(
                '            --validated-entry "$RUNNER_TEMP/validated-entry/entry" \\\n'
                '            --bundle "$GITHUB_WORKSPACE/bcr-entry.bundle"',
                '            --bundle "$GITHUB_WORKSPACE/bcr-entry.bundle"', 1),
                "compare the proposed entry"),
            "unverified bundle": (path, workflow.replace(
                "tools.bcr_release bundle-entry", "tools.bcr_release stage-entry", 1),
                "compare the proposed entry"),
            "inline recheck": (path, workflow.replace(
                "tools.bcr_release verify-bundle", "tools.bcr_release verify-generated", 1),
                "recheck the commit"),
            "untrusted bundle code": (path, workflow.replace(
                "PYTHONPATH=trusted python3 -B -m tools.bcr_release bundle-entry",
                "PYTHONPATH=this python3 -B -m tools.bcr_release bundle-entry", 1),
                "compare the proposed entry"),
            "untrusted recheck code": (path, workflow.replace(
                "PYTHONPATH=trusted python3 -B -m tools.bcr_release verify-bundle",
                "PYTHONPATH=this python3 -B -m tools.bcr_release verify-bundle", 1),
                "recheck the commit"),
            "unpinned tree": (path, workflow.replace(
                ' --tree "$PREPARED_TREE"', "", 1), "recheck the commit"),
            "unpinned commit": (path, workflow.replace(
                "PREPARED_COMMIT: ${{ needs.promote-entry.outputs.commit }}",
                "PREPARED_COMMIT: ${{ needs.validate.outputs.source_commit }}", 1),
                "recheck the commit"),
            "unpinned bundle digest": (path, workflow.replace(
                '--bundle-sha256 "$BUNDLE_SHA256" ', "", 1), "recheck the commit"),
            "unpinned base": (path, workflow.replace(
                "PREPARED_BASE: ${{ needs.promote-entry.outputs.base }}\n          PREPARED_COMMIT",
                "PREPARED_BASE: ${{ needs.validate.outputs.tag }}\n          PREPARED_COMMIT", 1),
                "recheck the commit"),
            "unchecked fork commit": (path, workflow.replace(
                '            --validated-entry "$RUNNER_TEMP/validated-entry/entry" \\\n'
                '            --proposed-commit', '            --proposed-commit', 1),
                "recheck the commit"),
            "ignored entry fetch": (path, workflow.replace(
                "      - name: Fetch the admission-validated entry the record names\n",
                "      - name: Fetch the admission-validated entry the record names\n"
                "        continue-on-error: true\n", 1), "ignores a failure"),
            "entry fetch or true": (path, workflow.replace(
                '--work "$RUNNER_TEMP/validated-entry"\n',
                '--work "$RUNNER_TEMP/validated-entry" || true\n', 1), "ignores a failure"),
            "plan or true": (path, workflow.replace(
                '            --github-output "$GITHUB_OUTPUT"\n',
                '            --github-output "$GITHUB_OUTPUT" || true\n', 1), "ignores a failure"),
            "input digest": (path, workflow.replace(
                BCR_RECORD_DIGEST, "APPROVED_SOURCE_SHA256: ${{ inputs.record_sha256 }}", 1),
                "compare the proposed entry"),
        })

    def test_docs_deploy_promotes_exactly_the_selected_retained_build(self):
        self.assertEqual([], _docs_deploy_violations(self.supply_chain_files[DOCS_WORKFLOW]))

    def test_docs_deploy_policy_rejects_builds_and_unselected_artifacts(self):
        path = DOCS_WORKFLOW
        workflow = self.supply_chain_files[path]
        download = "      - name: Download the retained Pages artifact\n"
        self._assert_rejected(lambda files, path: _docs_deploy_violations(files[path]), {
            "doxygen": (path, workflow.replace(
                download, "      - name: Rebuild\n        run: tools/build_docs.sh\n" + download,
                1), "deploy builds"),
            "upload": (path, workflow.replace(
                download, "      - uses: actions/upload-pages-artifact@v5\n" + download, 1),
                "deploy builds"),
            "input artifact": (path, workflow.replace(
                "artifact-ids: ${{ steps.select.outputs.artifact_id }}",
                "artifact-ids: ${{ inputs.artifact_id }}", 1), "did not name"),
            "no record selection": (path, workflow.replace(
                "python3 -B -m tools.release_candidate docs-selection",
                "python3 -B -m tools.pages_artifact inputs", 1), "select from the candidate record"),
            "no record attestation": (path, workflow.replace(
                "          gh attestation verify", "          echo", 1),
                "record attestation"),
            "unpinned source digest": (path, workflow.replace(
                '--source-digest "$RECORD_COMMIT"', '--source-digest "$GITHUB_SHA"', 1),
                "record attestation"),
            "unpinned source ref": (path, workflow.replace(
                '"$RECORD_COMMIT" --source-ref refs/heads/main', '"$RECORD_COMMIT"', 1),
                "record attestation"),
            "unnamed record run": (path, workflow.replace(
                '--record-run "$RECORD_RUN" --record-sha256', '--record-sha256', 1),
                "fetch the named candidate record"),
            "ignored attestation": (path, workflow.replace(
                "      - name: Verify the candidate record attestation\n",
                "      - name: Verify the candidate record attestation\n"
                "        continue-on-error: true\n", 1), "ignores a failure"),
            "disabled attestation": (path, workflow.replace(
                "        if: inputs.selection == 'record'\n        env:\n"
                "          GH_TOKEN: ${{ github.token }}\n          RECORD_PATH:",
                "        if: inputs.selection == 'record' && false\n        env:\n"
                "          GH_TOKEN: ${{ github.token }}\n          RECORD_PATH:", 1),
                "record attestation"),
            "disabled record fetch": (path, workflow.replace(
                "        id: record\n        if: inputs.selection == 'record'\n",
                "        id: record\n        if: inputs.selection == 'record' && false\n", 1),
                "fetch the named candidate record"),
            "attestation or true": (path, workflow.replace(
                "--source-ref refs/heads/main > /dev/null",
                "--source-ref refs/heads/main > /dev/null || true", 1), "ignores a failure"),
            "other selected record": (path, workflow.replace(
                "          SELECTION: ${{ inputs.selection }}\n"
                "          RECORD_RUN: ${{ inputs.record_run }}\n"
                "          RECORD_PATH: ${{ steps.record.outputs.record_path }}\n",
                "          SELECTION: ${{ inputs.selection }}\n"
                "          RECORD_RUN: ${{ inputs.record_run }}\n"
                "          RECORD_PATH: ${{ inputs.record_run }}\n", 1),
                "select from the candidate record"),
            "other attested record": (path, workflow.replace(
                "          RECORD_COMMIT: ${{ steps.record.outputs.record_commit }}\n",
                "          RECORD_COMMIT: ${{ github.sha }}\n", 1),
                "record attestation"),
            "attestation after selection": (path, workflow.replace(
                "      - name: Verify the candidate record attestation\n",
                "      - name: Check the candidate record attestation\n", 1).replace(
                "      - name: Download the retained Pages artifact\n",
                "      - name: Verify the candidate record attestation\n        run: true\n"
                "      - name: Download the retained Pages artifact\n", 1),
                "select and verify before"),
            "unverified archive": (path, workflow.replace(
                "python3 -B -m tools.pages_artifact verify", "true", 1),
                "verify the selected archive"),
            "input promotion": (path, workflow.replace(
                "          DOC_ARTIFACT_ID: ${{ steps.select.outputs.artifact_id }}\n"
                "          DOC_SOURCE_RUN_ID: ${{ steps.select.outputs.run_id }}\n"
                "          DOC_SOURCE_ATTEMPT: ${{ steps.select.outputs.attempt }}\n"
                "          DOC_SOURCE_COMMIT: ${{ steps.select.outputs.source_commit }}\n"
                "        with:",
                "          DOC_ARTIFACT_ID: ${{ inputs.artifact_id }}\n        with:", 1),
                "promotes an artifact"),
            "push deploy": (path, workflow.replace(
                "if: github.event_name == 'workflow_dispatch' && github.ref == 'refs/heads/main'",
                "if: github.ref == 'refs/heads/main'", 1), "manual dispatch on main"),
        })

    def test_git_dependencies_use_release_tags_or_explicit_commits(self):
        deps = _read("third_party/bazel/non_bcr_deps.bzl")
        for block in re.findall(r"(?:new_)?git_repository\((.*?)\n\s*\)", deps, re.DOTALL):
            if "remote =" not in block:
                continue
            self.assertRegex(
                block,
                r'(?:commit\s*=\s*"[0-9a-f]{40}"|tag\s*=\s*"v?\d+(?:\.\d+){1,2}")',
            )

    def test_fuzzer_variant_tags_derive_matching_ubsan_lanes(self):
        rules = _read("build_defs/rules.bzl")
        self.assertIn('"fuzz_text_full" in common_tags', rules)
        self.assertIn('ubsan_tags.append("fuzz_ubsan_text_full")', rules)
        self.assertIn('"fuzz_geode" in common_tags', rules)
        self.assertIn('ubsan_tags.append("fuzz_ubsan_geode")', rules)

    def test_fuzz_soaks_are_tagged_for_pr_exclusion_with_a_hang_guard_timeout(self):
        rules = _read("build_defs/rules.bzl")
        self.assertIn('soak_tags = common_target_tags + ["fuzz_soak"]', rules)
        self.assertIn("        tags = soak_tags,\n", rules)
        self.assertIn("        per_input_timeout_seconds = 25,\n", rules)
        self.assertIn("_ASAN_FUZZER_PER_INPUT_TIMEOUT_SECONDS = 60\n", rules)
        # The hang guard does not hide a slow mutated input: the launcher replays its report.
        self.assertIn("_SOAK_SLOW_UNIT_SECONDS = 10\n", rules)
        self.assertIn('["-report_slow_units=%d" % _SOAK_SLOW_UNIT_SECONDS]', rules)
        self.assertIn('"//build_defs:asan_fuzzer_enabled": [', rules)
        # Every nightly soak runs under the ASan fuzzer config, so it gets the sanitizer timeout.
        # The Fuzz workflow runs only its `fuzz-lane*` configs, each of which carries both.
        commands = [line for line in self.fuzz.splitlines() if "bazelisk test" in line]
        self.assertEqual(len(commands), 7)
        for line in commands:
            # Exactly one lane config and the lane's targets: nothing can override the lane.
            lane = re.fullmatch(r"\s*run: bazelisk test --config=(fuzz-lane[a-z-]*) "
                                r"(?://\.\.\.|//tools/ci:image_loader_production_soak)", line)
            self.assertIsNotNone(lane, line)
            flags = _lane_config_flags(lane.group(1))
            self.assertIn("--config=fuzz-soak", flags)
            self.assertIn("--config=asan-fuzzer", flags)

    def test_buildbuddy_keys_follow_event_and_shared_trust_gate(self):
        self.assertEqual([], _buildbuddy_violations(self.supply_chain_files))
        self.assertEqual(SELF_HOSTED_BUILDBUDDY_JOBS, BUILDBUDDY_JOBS & set(SELF_HOSTED_JOBS))
        # A workflow that only runs for pull requests has no main writer of its own.
        sanitizers_pr = self.supply_chain_files[".github/workflows/sanitizers-pr.yml"]
        self.assertEqual({"pull_request"}, _triggers(sanitizers_pr))
        # The path filters of the BuildBuddy lanes' workflows, as the policy reads them.
        filtered = {path: _trigger_paths(self.supply_chain_files[path])
                    for path in sorted({path for path, _ in BUILDBUDDY_JOBS})}
        self.assertEqual({".github/workflows/editor_wasm.yml": {"pull_request", "push"},
                          ".github/workflows/sanitizers-pr.yml": {"pull_request"}},
                         {path: set(paths) for path, paths in filtered.items() if paths})
        self.assertIn("tools/ci/**",
                      filtered[".github/workflows/sanitizers-pr.yml"]["pull_request"])
        self.assertEqual({"push": ['["a/**"]']},
                         _trigger_paths('on:\n  push:\n    paths: ["a/**"]\njobs:\n'))
        for layout in ('on: {push: {paths: ["a/**"]}}\n',
                       'on:\n  pull_request: {paths: ["a/**"]}\n'):
            with self.subTest(layout=layout), self.assertRaises(ValueError):
                _trigger_paths(layout + "jobs:\n")

    def test_write_key_environment_resolves_only_for_main_and_trusted_pull_requests(self):
        # name: (event, ref, trusted_pr, renovate_pr, expected environment binding)
        runs = {
            "main push": ("push", "refs/heads/main", "false", "false", True),
            "main schedule": ("schedule", "refs/heads/main", "false", "false", True),
            "main dispatch": ("workflow_dispatch", "refs/heads/main", "false", "false", True),
            "trusted pull request": ("pull_request", "refs/pull/1/merge", "true", "false", True),
            "Renovate pull request": ("pull_request", "refs/pull/1/merge", "false", "true", False),
            "untrusted or fork pull request": (
                "pull_request", "refs/pull/1/merge", "false", "false", False),
            "branch push": ("push", "refs/heads/topic", "false", "false", False),
            "pull request against main ref": ("pull_request", "refs/heads/main", "false",
                                              "false", False),
        }
        for path, name in sorted(BUILDBUDDY_JOBS):
            workflow = self.supply_chain_files[path]
            job = _jobs(workflow)[name]
            condition = _write_environment_condition(job)
            self.assertIsNotNone(condition, "%s:%s" % (path, name))
            self.assertNotIn("renovate_pr", condition)
            gates = _trust_gates(workflow, job)
            for run, (event, ref, trusted, renovate, expected) in runs.items():
                # A lane without a trust gate (schedule and dispatch only) never writes for a
                # pull request.
                expected = expected and (event != "pull_request" or bool(gates))
                with self.subTest(job="%s:%s" % (path, name), run=run):
                    values = {gate: {"trusted_pr": trusted, "renovate_pr": renovate}
                              for gate in gates}
                    self.assertEqual(expected,
                                     bool(_evaluate_condition(condition, event, ref, values)))

    def test_write_environment_policy_rejects_widened_bindings(self):
        files = self.supply_chain_files
        main = ".github/workflows/main.yml"
        wasm = ".github/workflows/editor_wasm.yml"
        nightly = ".github/workflows/github_hosted_nightly.yml"
        lint = ".github/workflows/lint.yml"
        docs = ".github/workflows/deploy_docs.yaml"
        bcr = ".github/workflows/publish_bcr.yml"
        main_env = ("      name: ${{ ((github.event_name != 'pull_request' && github.ref == "
                    "'refs/heads/main') || needs.gatekeeper.outputs.trusted_pr == 'true') && "
                    "'buildbuddy-write' || '' }}\n")
        self._assert_rejected(lambda files, path: _buildbuddy_violations(files), {
            "unconditional environment": (main, files[main].replace(
                main_env, "      name: buildbuddy-write\n", 1), "outside main and trusted"),
            "environment for Renovate": (main, files[main].replace(
                main_env, main_env.replace("trusted_pr", "renovate_pr"), 1),
                "outside main and trusted"),
            "environment for every pull request": (main, files[main].replace(
                main_env, main_env.replace(
                    "needs.gatekeeper.outputs.trusted_pr == 'true'",
                    "github.event_name == 'pull_request'"), 1), "outside main and trusted"),
            "deployment record": (main, files[main].replace(
                main_env + "      deployment: false\n", main_env, 1), "outside main and trusted"),
            "no environment": (nightly, files[nightly].replace(
                "      deployment: false\n", "", 1).replace(
                "    environment:\n", "    environment-removed:\n", 1),
                "outside main and trusted"),
            "trust job without the shared rule": (wasm, files[wasm].replace(
                "        uses: ./.github/actions/pr-trust\n",
                "        run: echo trusted_pr=true >> \"$GITHUB_OUTPUT\"\n", 1),
                "outside main and trusted"),
            "writer step wider than environment": (nightly, files[nightly].replace(
                "        if: github.event_name != 'pull_request' && github.ref == "
                "'refs/heads/main'\n        uses: ./.github/actions/buildbuddy-cache\n",
                "        if: github.ref == 'refs/heads/main'\n"
                "        uses: ./.github/actions/buildbuddy-cache\n", 1),
                "neither main nor a trusted"),
            "environment in another job": (lint, files[lint].replace(
                "  lint:\n", "  lint:\n    environment:\n      name: buildbuddy-write\n"
                "      deployment: false\n", 1), "unreviewed environment"),
            "inline environment in another job": (lint, files[lint].replace(
                "  lint:\n", "  lint:\n    environment: BuildBuddy-Write\n", 1),
                "unreviewed environment"),
            "format() name in another job": (lint, files[lint].replace(
                "  lint:\n", "  lint:\n    environment:\n"
                "      name: ${{ format('{0}-{1}', 'buildbuddy', 'write') }}\n", 1),
                "unreviewed environment"),
            "vars name in another job": (lint, files[lint].replace(
                "  lint:\n", "  lint:\n    environment:\n      name: ${{ vars.ENVIRONMENT }}\n",
                1), "unreviewed environment"),
            "reviewed job renamed environment": (docs, files[docs].replace(
                "      name: github-pages\n", "      name: ${{ vars.PAGES_ENV }}\n", 1),
                "unreviewed environment"),
            "reviewed job switched to write environment": (bcr, files[bcr].replace(
                "    environment: bcr-fork-preparation\n",
                "    environment: BUILDBUDDY-WRITE\n", 1),
                "unreviewed environment"),
        })
        # Case differences in a reviewed literal name are the same environment.
        cased = files[docs].replace("      name: github-pages\n", "      name: GitHub-Pages\n", 1)
        self.assertNotEqual(cased, files[docs])
        self.assertEqual([], _buildbuddy_violations({**files, docs: cased}))

    def test_buildbuddy_policy_rejects_widened_key_access(self):
        files = self.supply_chain_files
        main = ".github/workflows/main.yml"
        wasm = ".github/workflows/editor_wasm.yml"
        nightly = ".github/workflows/github_hosted_nightly.yml"
        sanitizers_pr = ".github/workflows/sanitizers-pr.yml"
        action_path = '      - "%s"\n' % BUILDBUDDY_ACTION_FILES
        writer_if = ("        if: (github.event_name != 'pull_request' && github.ref == "
                     "'refs/heads/main') || needs.gatekeeper.outputs.trusted_pr == 'true'\n")
        reader_if = "        if: needs.gatekeeper.outputs.renovate_pr == 'true'\n"
        read_key = "${{ secrets.BUILDBUDDY_API_KEY_READONLY }}"
        write_key = "${{ secrets.BUILDBUDDY_API_KEY }}"
        cases = {
            "write key for every pull request": (main, files[main].replace(
                writer_if, "        if: always()\n", 1), "neither main nor a trusted"),
            "write key for Renovate": (main, files[main].replace(
                writer_if, writer_if.replace("trusted_pr", "renovate_pr"), 1),
                "neither main nor a trusted"),
            "write key without a step condition": (main, files[main].replace(
                writer_if, "", 1), "neither main nor a trusted"),
            "write key in the Renovate reader": (main, files[main].replace(
                "read-only-api-key: " + read_key, "api-key: " + write_key, 1),
                "neither main nor a trusted"),
            "read key for any pull request": (main, files[main].replace(
                reader_if, "        if: github.event_name == 'pull_request'\n", 1),
                "not a Renovate pull request"),
            "trust gate not from pr-trust": (main, files[main].replace(
                "      trusted_pr: ${{ steps.pr_trust.outputs.trusted_pr }}\n",
                "      trusted_pr: ${{ github.actor == 'jwmcglynn' }}\n", 1),
                "neither main nor a trusted"),
            "trusted input from another gate": (main, files[main].replace(
                "          trusted-pr: ${{ needs.gatekeeper.outputs.trusted_pr }}\n",
                "          trusted-pr: 'true'\n", 1), "neither main nor a trusted"),
            "key outside the action": (nightly, files[nightly].replace(
                "      - name: Build //...\n", "      - name: Build //...\n        env:\n"
                "          KEY: ${{ secrets.BUILDBUDDY_API_KEY }}\n", 1), "outside the action"),
            "pull request lane without Renovate reader": (main, files[main].replace(
                "      - name: Configure BuildBuddy read-only cache\n" + reader_if +
                "        uses: ./.github/actions/buildbuddy-cache\n        with:\n"
                "          read-only-api-key: " + read_key + "\n"
                "          renovate-pr: ${{ needs.gatekeeper.outputs.renovate_pr }}\n"
                "          download-outputs: minimal\n\n", "", 1), "does not pair one writer"),
            "no download choice": (nightly, files[nightly].replace(
                "          download-outputs: minimal\n", "", 1),
                "does not choose --remote_download_outputs"),
            "Bazel before the cache": (wasm, files[wasm].replace(
                "      - name: Restore Bazel action cache\n", "      - name: Early build\n"
                "        run: bazel build //donner/base\n\n"
                "      - name: Restore Bazel action cache\n", 1),
                "runs Bazel before configuring BuildBuddy"),
            "wrapper before the cache": (wasm, files[wasm].replace(
                "      - name: Restore Bazel action cache\n", "      - name: Early browser run\n"
                "        run: bash tools/run-browser-ci.sh\n\n"
                "      - name: Restore Bazel action cache\n", 1),
                "runs Bazel before configuring BuildBuddy"),
            "lane dropped": (wasm, files[wasm].replace(
                "uses: ./.github/actions/buildbuddy-cache", "uses: ./.github/actions/apt-install"),
                "BuildBuddy lanes changed"),
            "fallback turned off for one step": (main, files[main].replace(
                "bazelisk build --config=ci $BAZEL_MACOS_BUILD_FLAGS $TARGETS",
                "bazelisk build --config=ci --remote_local_fallback=false "
                "$BAZEL_MACOS_BUILD_FLAGS $TARGETS", 1),
                "overrides the BuildBuddy local fallback"),
            "fallback turned off for the job": (nightly, files[nightly].replace(
                "        linker: [default, gold]\n    steps:\n",
                "        linker: [default, gold]\n    env:\n      FLAGS: "
                "--noincompatible_remote_local_fallback_for_remote_cache\n    steps:\n", 1),
                "overrides the BuildBuddy local fallback"),
            "fallback turned off for the workflow": (nightly, files[nightly].replace(
                '  ASPECT_TOOLS_TELEMETRY: "-all"\n', '  ASPECT_TOOLS_TELEMETRY: "-all"\n'
                "  FLAGS: --remote_local_fallback=false\n", 1),
                "overrides the BuildBuddy local fallback"),
            "push paths skip the action": (wasm, files[wasm].replace(
                action_path, "", 1), "editor_wasm.yml has a BuildBuddy lane, but its push paths"),
            "pull request paths skip the action": (sanitizers_pr, files[sanitizers_pr].replace(
                action_path, "", 1),
                "sanitizers-pr.yml has a BuildBuddy lane, but its pull_request paths"),
            "flow-style paths": (sanitizers_pr, files[sanitizers_pr].replace(
                "    paths:\n", '    paths: ["%s"]\n    paths-moved:\n' % BUILDBUDDY_ACTION_FILES,
                1), "its pull_request paths skip the action"),
        }
        self._assert_rejected(lambda files, path: _buildbuddy_violations(files), cases)
        cleanup = BUILDBUDDY_CLEANUP
        linux_writer = ("      - name: Configure BuildBuddy remote cache\n" + writer_if +
                        "        uses: ./.github/actions/buildbuddy-cache\n        with:\n"
                        "          api-key: " + write_key + "\n"
                        "          trusted-pr: ${{ needs.gatekeeper.outputs.trusted_pr }}\n"
                        "          renovate-pr: ${{ needs.gatekeeper.outputs.renovate_pr }}\n"
                        "          download-outputs: minimal\n\n")
        linux_checkout = ("    steps:\n      - name: Checkout\n        uses: actions/checkout@v7\n"
                          "        with:\n          persist-credentials: false\n\n"
                          "      - name: Record target selection\n")
        self.assertEqual(1, files[main].count(linux_checkout))
        self._assert_rejected(lambda files, path: _buildbuddy_violations(files), {
            "another self-hosted lane": (main, files[main].replace(
                linux_checkout, linux_checkout.replace(
                    "      - name: Record target selection\n",
                    linux_writer + "      - name: Record target selection\n"), 1),
                "uses BuildBuddy on a self-hosted runner"),
            "self-hosted key left on the runner": (main, files[main].replace(
                "\n" + cleanup, "\n", 1), "does not remove the BuildBuddy key"),
            "cleanup skipped on failure": (main, files[main].replace(
                cleanup, cleanup.replace("if: always()", "if: success()"), 1),
                "does not remove the BuildBuddy key"),
            "self-hosted Renovate reader": (main, files[main].replace(
                "          cache-partition: ${{ steps.xcode.outputs.cache_partition }}\n"
                "          download-outputs: minimal\n\n"
                "      - name: Prepare self-hosted macOS toolchain\n",
                "          cache-partition: ${{ steps.xcode.outputs.cache_partition }}\n"
                "          download-outputs: minimal\n\n"
                "      - name: Configure BuildBuddy read-only cache\n" + reader_if +
                "        uses: ./.github/actions/buildbuddy-cache\n        with:\n"
                "          read-only-api-key: " + read_key + "\n"
                "          renovate-pr: ${{ needs.gatekeeper.outputs.renovate_pr }}\n"
                "          download-outputs: minimal\n\n"
                "      - name: Prepare self-hosted macOS toolchain\n", 1),
                "does not pair one writer with no reader"),
            "Bazel before the self-hosted cache": (main, files[main].replace(
                "      # build fails here rather than building with another Xcode.\n"
                "      - name: Select pinned Xcode\n",
                "      # build fails here rather than building with another Xcode.\n"
                "      - name: Early build\n        run: bazelisk --nohome_rc build //donner/base\n\n"
                "      - name: Select pinned Xcode\n", 1),
                "runs Bazel before configuring BuildBuddy"),
        })

    def _run_buildbuddy_action(self, root, *, event, ref="refs/heads/main", write="",
                               read="", trusted="false", renovate="false",
                               download="minimal", curl_status=0, **overrides):
        script = textwrap.dedent(_run_bodies(
            self.supply_chain_files[".github/actions/buildbuddy-cache/action.yml"])[0])
        workspace = root / "workspace"
        temp = root / "runner-temp"
        tools = root / "bin"
        for directory in (workspace, temp, tools):
            directory.mkdir(exist_ok=True)
        (workspace / ".buildbuddy.bazelrc").unlink(missing_ok=True)
        curl = tools / "curl"
        curl.write_text("#!/bin/sh\nexit %d\n" % curl_status, encoding="utf-8")
        curl.chmod(0o755)
        env = {"PATH": "%s:%s" % (tools, os.environ["PATH"]), "GITHUB_WORKSPACE": str(workspace),
               "RUNNER_TEMP": str(temp), "RUNNER_ENVIRONMENT": "github-hosted",
               "GITHUB_OUTPUT": str(root / "output"), "REMOTE_EXECUTION": "false",
               "RUNNER_OS": "Linux",
               "ImageOS": "ubuntu24", "ImageVersion": "20261005.1", "RUNNER_ARCH": "X64",
               "WRITE_KEY": write, "READ_KEY": read, "TRUSTED_PR": trusted,
               "RENOVATE_PR": renovate, "DOWNLOAD_OUTPUTS": download, "EVENT_NAME": event,
               "REF": ref}
        env.update(overrides)
        result = subprocess.run(["bash", "-c", script], env=env, capture_output=True,
                                text=True, timeout=test_timing.STALL_GUARD_SECONDS,
                                check=False)
        fragment = workspace / ".buildbuddy.bazelrc"
        helper = temp / "buildbuddy" / "helper"
        credential = None
        if helper.exists():
            # Run the credential helper as Bazel does: `get`, with the request on stdin.
            answer = subprocess.run([str(helper), "get"], input='{"uri": "grpcs://x"}',
                                    capture_output=True, text=True,
                                    timeout=test_timing.STALL_GUARD_SECONDS, check=True)
            credential = answer.stdout
        return (result.returncode, fragment.read_text() if fragment.exists() else None,
                credential, result.stdout)

    def test_buildbuddy_action_selects_keys_by_event_and_fails_closed(self):
        write, read = "write-key-fixture", "read-key-fixture"
        cases = {
            # name: (arguments, expected exit, expected upload flag or None for no cache)
            "main push writes": (dict(event="push", write=write), 0, "write"),
            "main schedule writes": (dict(event="schedule", write=write), 0, "write"),
            "main dispatch writes": (dict(event="workflow_dispatch", write=write), 0, "write"),
            "trusted pull request writes": (dict(
                event="pull_request", ref="refs/pull/1/merge", write=write, trusted="true"),
                0, "write"),
            "Renovate pull request reads": (dict(
                event="pull_request", ref="refs/pull/1/merge", read=read, renovate="true"),
                0, "read"),
            "no key is inert": (dict(event="pull_request", ref="refs/pull/1/merge"), 0, None),
            "branch push with write key": (dict(
                event="push", ref="refs/heads/topic", write=write), 1, None),
            "untrusted pull request with write key": (dict(
                event="pull_request", ref="refs/pull/1/merge", write=write), 1, None),
            "Renovate pull request with write key": (dict(
                event="pull_request", ref="refs/pull/1/merge", write=write, trusted="true",
                renovate="true"), 1, None),
            "pull_request_target with write key": (dict(
                event="pull_request_target", write=write, trusted="true"), 1, None),
            "trusted pull request with read key": (dict(
                event="pull_request", ref="refs/pull/1/merge", read=read, trusted="true"),
                1, None),
            "main push with read key": (dict(event="push", read=read), 1, None),
            "both keys": (dict(event="push", write=write, read=read), 1, None),
            "self-hosted runner on main": (dict(
                event="push", write=write, RUNNER_ENVIRONMENT="self-hosted", RUNNER_OS="macOS"),
                1, None),
            "self-hosted macOS trusted pull request writes": (dict(
                event="pull_request", ref="refs/pull/1/merge", write=write, trusted="true",
                RUNNER_ENVIRONMENT="self-hosted", RUNNER_OS="macOS",
                CACHE_PARTITION="ubuntu24-20261005.1-X64"), 0, "write"),
            "self-hosted macOS without a partition": (dict(
                event="pull_request", ref="refs/pull/1/merge", write=write, trusted="true",
                RUNNER_ENVIRONMENT="self-hosted", RUNNER_OS="macOS"), 1, None),
            "key that could break the helper's JSON": (dict(
                event="push", write='key"]},"x":["y'), 1, None),
            "self-hosted Linux trusted pull request": (dict(
                event="pull_request", ref="refs/pull/1/merge", write=write, trusted="true",
                RUNNER_ENVIRONMENT="self-hosted", RUNNER_OS="Linux"), 1, None),
            "self-hosted macOS Renovate reader": (dict(
                event="pull_request", ref="refs/pull/1/merge", read=read, renovate="true",
                RUNNER_ENVIRONMENT="self-hosted", RUNNER_OS="macOS"), 1, None),
            "unknown runner environment": (dict(
                event="push", write=write, RUNNER_ENVIRONMENT=""), 1, None),
            "unknown download mode": (dict(event="push", write=write, download="some"), 1, None),
            "unreachable cache": (dict(event="push", write=write, curl_status=7), 0, None),
            "TLS failure": (dict(event="push", write=write, curl_status=35), 0, None),
            "unknown runner image": (dict(event="push", write=write, ImageVersion=""), 0, None),
        }
        for name, (arguments, status, mode) in cases.items():
            with self.subTest(case=name), tempfile.TemporaryDirectory() as directory:
                code, fragment, credential, output = self._run_buildbuddy_action(
                    Path(directory), **arguments)
                self.assertEqual(status, code, output)
                if mode is None:
                    self.assertIsNone(fragment)
                    continue
                key = write if mode == "write" else read
                self.assertNotIn(key, fragment)
                self.assertNotIn("remote_header", fragment)
                self.assertEqual({"headers": {"x-buildbuddy-api-key": [key]}},
                                 json.loads(credential))
                lines = fragment.splitlines()
                self.assertIn("build --remote_cache=grpcs://remote.buildbuddy.io", lines)
                self.assertIn("build --remote_instance_name=ubuntu24-20261005.1-X64", lines)
                self.assertIn("build --remote_download_outputs=minimal", lines)
                self.assertIn(r"build --remote_download_regex=.*/rules_playwright[+][^/]*/browsers/.*",
                              lines)
                self.assertFalse(any("remote_executor" in line or "bes_" in line
                                     for line in lines))
                upload = ("build --remote_upload_local_results" if mode == "write"
                          else "build --noremote_upload_local_results")
                self.assertIn(upload, lines)
                self.assertEqual(1, sum("upload_local_results" in line for line in lines))
                self.assertIn("build --credential_helper=remote.buildbuddy.io="
                              "%s/runner-temp/buildbuddy/helper" % directory, lines)
                self.assertFalse(any(line.startswith("import") for line in lines))
                self.assertEqual(sorted(BUILDBUDDY_FALLBACK), _fallback_lines(fragment))

    def test_buildbuddy_execution_config_preserves_local_test_runtime(self):
        # The cloud config changes only where actions run. Its exact contents are pinned, so a
        # toolchain, target selection, tag filter or test environment cannot hide in it and
        # change what main's hosted Linux steps build or test.
        self.assertEqual([
            "build:buildbuddy-re --extra_execution_platforms=//tools/ci:buildbuddy_linux_x86_64",
            "build:buildbuddy-re --platforms=//tools/ci:buildbuddy_linux_x86_64",
            "build:buildbuddy-re --spawn_strategy=remote",
            'build:buildbuddy-re --strategy_regexp="^Executing genrule '
            '//donner/editor:editor_build_info_txt$=local"',
            "build:buildbuddy-re --strategy=TestRunner=local",
            "build:buildbuddy-re --noremote_local_fallback",
            "build:buildbuddy-re --remote_timeout=600s",
            "build:buildbuddy-re --jobs=50",
        ], [line for line in _read(".bazelrc").splitlines()
            if re.match(r"\s*[a-z]+:buildbuddy-re\b", line)])
        main = self.supply_chain_files[".github/workflows/main.yml"]
        probe = _step_body(main, "Verify BuildBuddy remote execution")
        self.assertIn("--remote_accept_cached=false --disk_cache=", probe)
        self.assertIn("--copt=-DBUILDBUDDY_RE_PROBE_${GITHUB_RUN_ID}_${GITHUB_RUN_ATTEMPT}", probe)
        self.assertIn('.runner == "remote" and .cacheHit != true', probe)
        self.assertIn("bazelisk build --config=ci --config=latest_llvm ${BUILDBUDDY_FLAGS} //...",
                      probe)
        # Every hosted Linux Bazel step selects the declared LLVM toolchain itself, so a run
        # without the cloud credential builds what main builds. Only the linker canary keeps the
        # runner's toolchain, because it checks the runner's non-lld linker.
        linux = _jobs(main)["linux"]
        bazel_steps = {name: _step_body(linux, name)
                       for name in re.findall(r"(?m)^      - name: (.*)$", linux)
                       if "bazelisk " in _step_body(linux, name)}
        self.assertEqual({"Linker canary (non-lld linker support)"},
                         {name for name, body in bazel_steps.items()
                          if "--config=latest_llvm" not in body})
        for name, body in bazel_steps.items():
            code = "\n".join(line for line in body.splitlines()
                             if not line.lstrip().startswith("#"))
            joined = re.sub(r"\\\n\s*", " ", code)
            for command in re.findall(r"\bbazel(?:isk)? \S.*", joined):
                expected = ("--linkopt=-fuse-ld=gold"
                            if name.startswith("Linker canary") else "--config=latest_llvm")
                self.assertIn(expected, command, name)
            self.assertNotIn("llvm_latest", body, name)
        # The cloud target platform carries no constraint beyond x86_64 Linux, so it selects the
        # same toolchains and select() branches as a run on the hosted runner.
        platform = re.search(r'(?ms)^platform\(\n    name = "buildbuddy_linux_x86_64",\n(.*?)^\)$',
                             _read("tools/ci/BUILD.bazel")).group(1)
        constraints = re.search(r"(?ms)^    constraint_values = \[\n(.*?)^    \],$", platform)
        self.assertEqual(["@platforms//cpu:x86_64", "@platforms//os:linux"],
                         re.findall(r'"([^"]+)"', constraints.group(1)))
        self.assertEqual(["constraint_values", "exec_properties", "name", "visibility"],
                         sorted(re.findall(r"(?m)^    ([a-z_]+) =", "    name =\n" + platform)))

    def test_buildbuddy_remote_execution_requires_linux_writer(self):
        cases = {
            "Linux writer": (dict(), 0),
            "macOS": (dict(RUNNER_OS="macOS"), 1),
            "ARM64": (dict(RUNNER_ARCH="ARM64"), 1),
            "reader": (dict(event="pull_request", write="", read="read-fixture",
                            renovate="true"), 1),
            "invalid value": (dict(REMOTE_EXECUTION="yes"), 1),
            "trusted PR writer": (dict(event="pull_request", ref="refs/pull/1/merge",
                                       trusted="true"), 0),
            "self-hosted macOS writer": (dict(event="pull_request", ref="refs/pull/1/merge",
                                              trusted="true", RUNNER_ENVIRONMENT="self-hosted",
                                              RUNNER_OS="macOS"), 1),
        }
        for name, (overrides, expected) in cases.items():
            with self.subTest(case=name), tempfile.TemporaryDirectory() as directory:
                args = dict(event="push", write="write-fixture", REMOTE_EXECUTION="true")
                args.update(overrides)
                code, fragment, _, output = self._run_buildbuddy_action(Path(directory), **args)
                self.assertEqual(expected, code, output)
                if expected:
                    self.assertIsNone(fragment)
                    continue
                # The fragment adds only the executor endpoint to the cloud config.
                self.assertEqual(
                    ["build:buildbuddy-re --remote_executor=grpcs://remote.buildbuddy.io"],
                    [line for line in fragment.splitlines() if "buildbuddy-re" in line])
                self.assertIn("bazel-flags=--config=buildbuddy-re",
                              (Path(directory) / "output").read_text().splitlines())
                self.assertFalse(any(line.startswith("build --remote_executor=")
                                     for line in fragment.splitlines()))

    def test_only_the_hosted_linux_writer_opts_into_remote_execution(self):
        # The action fails closed on the wrong runner, but a workflow could still hand the cloud
        # flags to another command. Pin who opts in and every command that consumes the flags.
        steps = list(_workflow_steps(self.supply_chain_files))
        opted_in = {key + (_REMOTE_EXECUTION_INPUT.search(step).group(1).strip(),)
                    for key, step in steps if _REMOTE_EXECUTION_INPUT.search(step)}
        consumers = {key for key, step in steps if "outputs.bazel-flags" in step}
        main = ".github/workflows/main.yml"
        self.assertEqual({(main, "linux", "Configure BuildBuddy remote cache", '"true"')},
                         opted_in)
        self.assertEqual({(main, "linux", step) for step in (
            "Verify BuildBuddy remote execution", "Build", "Test",
            "Test (hosted parity subset)")}, consumers)

    def test_bazelrc_leaves_the_buildbuddy_fallback_to_the_fragment(self):
        # A later rc line, a command section such as `test`, or a config such as --config=ci
        # outranks the fragment's plain `build` lines, so a fallback flag anywhere in .bazelrc
        # could turn the fallback off on every lane.
        self.assertIsNone(REMOTE_FALLBACK_FLAG.search("\n".join(
            line for line in _read(".bazelrc").splitlines()
            if not line.startswith("build:buildbuddy-re "))))

    def test_buildbuddy_cache_partition_replaces_only_the_image_identity(self):
        write = "write-key-fixture"
        partition = "macos-ARM64-xcode-27.1-27A9275"
        with tempfile.TemporaryDirectory() as directory:
            code, fragment, _, output = self._run_buildbuddy_action(
                Path(directory), event="push", write=write, CACHE_PARTITION=partition,
                ImageVersion="")
            self.assertEqual(0, code, output)
            self.assertIn("build --remote_instance_name=%s" % partition, fragment.splitlines())
        for name, overrides in {
            "self-hosted runner": dict(RUNNER_ENVIRONMENT="self-hosted"),
            "flag injection": dict(CACHE_PARTITION="macos-ARM64 --remote_executor=grpc://x"),
            "newline injection": dict(CACHE_PARTITION="macos-ARM64\nbuild --config=re"),
        }.items():
            arguments = dict(event="push", write=write, CACHE_PARTITION=partition)
            arguments.update(overrides)
            with self.subTest(case=name), tempfile.TemporaryDirectory() as directory:
                code, fragment, _, output = self._run_buildbuddy_action(
                    Path(directory), **arguments)
                self.assertEqual(1, code, output)
                self.assertIsNone(fragment)

    def test_sensitive_workflows_use_least_privilege_and_drop_checkout_credentials(self):
        paths = (
            ".github/workflows/badges.yaml",
            ".github/workflows/cmake.yml",
            ".github/workflows/lint.yml",
            ".github/workflows/release.yml",
            CANDIDATE_WORKFLOW,
            ".github/workflows/sanitizers-pr.yml",
        )
        for path in paths:
            workflow = self.supply_chain_files[path]
            with self.subTest(path=path):
                self.assertRegex(workflow, r"(?m)^permissions:\n  contents: read$")
                checkout_count = workflow.count("uses: actions/checkout@")
                self.assertGreater(checkout_count, 0)
                self.assertEqual(checkout_count, workflow.count("persist-credentials: false"))

        for path, workflow in self.supply_chain_files.items():
            checkout_count = workflow.count("uses: actions/checkout@")
            if checkout_count == 0:
                continue
            with self.subTest(path=path, invariant="checkout-credentials"):
                self.assertEqual(checkout_count, workflow.count("persist-credentials: false"))

    def test_heavy_security_workflows_do_not_gate_pull_requests(self):
        self.assertNotRegex(self.codeql, r"(?m)^  pull_request:")
        self.assertNotRegex(self.fuzz, r"(?m)^  pull_request:")

    def test_codeql_builds_preserve_standalone_tracing_and_tag_lanes(self):
        body = _step_body(self.codeql, "Build tagged C++ entry points")
        script = textwrap.dedent(_run_bodies(body)[0])
        tags = ("codeql_default", "codeql_text_full", "codeql_geode",
                "codeql_wasm", "codeql_wasm_geode")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            receipt = root / "arguments"
            bazel = root / "bazel"
            bazel.write_text(
                '#!/bin/bash\nprintf "%s\\n" "$@" >> "$ARGUMENT_RECEIPT"\n'
                'printf "\\0" >> "$ARGUMENT_RECEIPT"\n'
                'exit "${FAKE_BAZEL_EXIT:-0}"\n')
            bazel.chmod(0o755)
            env = {"PATH": directory + os.pathsep + os.defpath,
                   "ARGUMENT_RECEIPT": str(receipt)}
            command = ["/bin/bash", "-e", "-o", "pipefail", "-c", script]
            result = subprocess.run(command, cwd=root, env=env,
                                    capture_output=True, text=True,
                                    timeout=test_timing.STALL_GUARD_SECONDS)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            calls = [part.splitlines() for part in receipt.read_text().split("\0") if part]
            self.assertEqual(len(tags), len(calls))
            for args, tag in zip(calls, tags):
                with self.subTest(tag=tag):
                    self.assertEqual("build", args[0])
                    self.assertEqual("//...", args[-1])
                    for required in ("--spawn_strategy=local", "--experimental_output_paths=off",
                                     "--nouse_action_cache", "--noremote_accept_cached",
                                     "--noremote_upload_local_results", "--disk_cache=",
                                     "--build_tag_filters=" + tag):
                        self.assertIn(required, args)
            receipt.write_text("")
            result = subprocess.run(command, cwd=root, env={**env, "FAKE_BAZEL_EXIT": "17"},
                                    capture_output=True, text=True,
                                    timeout=test_timing.STALL_GUARD_SECONDS)
            self.assertEqual(17, result.returncode, result.stdout + result.stderr)
            self.assertEqual(1, receipt.read_text().count("\0"))
        self.assertNotRegex(body.replace("//...", ""), r"//[^\s]+")

    def test_fuzz_variants_are_selected_by_bazel_tags(self):
        expected = {
            "Test fuzzers (libFuzzer via LLVM 21 toolchain)": ("fuzz-lane", "fuzz_target"),
            "Test text-full fuzzers": ("fuzz-lane-text-full", "fuzz_text_full"),
            "Test Geode render fuzzers": ("fuzz-lane-geode", "fuzz_geode"),
        }
        for step, (lane, tag) in expected.items():
            body = _step_body(self.fuzz, step)
            self.assertIn("--config=%s //...\n" % lane, body)
            flags = _lane_config_flags(lane)
            self.assertIn("--test_tag_filters=%s" % tag, flags)
            self.assertIn("--build_tag_filters=%s" % tag, flags)
            self.assertNotRegex(body.replace("//...", ""), r"//[^\s]+")
        self.assertIn("--config=text-full", _lane_config_flags("fuzz-lane-text-full"))
        self.assertIn("--config=geode", _lane_config_flags("fuzz-lane-geode"))

    def test_production_image_decode_soak_runs_on_both_nightly_hosts(self):
        target = "//tools/ci:image_loader_production_soak"
        self.assertEqual(self.fuzz.count("Soak image loader at production decode budget"), 2)
        self.assertEqual(self.fuzz.count(target), 2)
        for job in ("linux", "macos"):
            section = self.fuzz.split("\n  %s:\n" % job, 1)[1]
            if job == "linux":
                section = section.split("\n  macos:\n", 1)[0]
            self.assertIn("--config=fuzz-lane-production-soak " + target, section)
        flags = _lane_config_flags("fuzz-lane-production-soak")
        self.assertIn("--config=asan-fuzzer", flags)
        self.assertIn("--config=fuzz-soak", flags)

    def test_ubsan_corpora_are_selected_by_bazel_tags(self):
        body = _step_body(self.sanitizers, "Replay untrusted-input fuzzer corpora with UBSan")
        lanes = {"ubsan-corpus": ("fuzz_ubsan", None),
                 "ubsan-corpus-text-full": ("fuzz_ubsan_text_full", "text-full"),
                 "ubsan-corpus-geode": ("fuzz_ubsan_geode", "geode")}
        self.assertEqual(re.findall(r"(?m)^\s*bazelisk test --config=(\S+) //\.\.\.$", body),
                         list(lanes))
        for lane, (tag, variant) in lanes.items():
            variant_flags = ["--config=%s" % variant] if variant else []
            self.assertEqual(_lane_config_flags(lane),
                             ["--config=ubsan", *variant_flags, "--test_output=errors",
                              "--test_tag_filters=%s" % tag, "--build_tag_filters=%s" % tag], lane)
        self.assertNotRegex(body.replace("//...", ""), r"//[^\s]+")

    def test_sanitizer_lanes_keep_their_scope_and_filters(self):
        # Exact lists: an extra or overriding flag in a lane fails too, and the order is pinned.
        filters = ["--build_tests_only", "--test_tag_filters=-fuzz_target,-ci-remote-gpu",
                   "--build_tag_filters=-fuzz_target,-ci-remote-gpu"]
        expected = {
            "asan-lane": ["--config=asan", "--test_output=errors", *filters],
            "asan-lane-no-leaks": ["--config=asan", "--test_output=errors",
                                   "--test_env=ASAN_OPTIONS=detect_leaks=0", *filters],
            "ubsan-lane": ["--config=ubsan", "--test_output=errors", *filters],
            "asan-geode-pr": ["--config=asan", "--config=geode", "--test_output=errors"],
        }
        for lane, flags in expected.items():
            self.assertEqual(_lane_config_flags(lane), flags, lane)
        asan = _step_body(self.sanitizers, "Test //donner/... with ASan")
        self.assertIn("--exclude-suite //tools/ci:asan_exclusions -- \\\n"
                      "            bazelisk test --config=asan-lane -- //donner/... "
                      "-//donner/svg/renderer/geode/...\n", asan)
        self.assertIn("bazelisk test --config=asan-lane-no-leaks \\\n"
                      "            //donner/svg/renderer/geode/... //tools/ci:asan_no_leak_tests\n",
                      asan)
        ubsan = _step_body(self.sanitizers, "Test //donner/... with UBSan")
        self.assertIn("--exclude-suite //tools/ci:ubsan_exclusions -- \\\n"
                      "            bazelisk test --config=ubsan-lane -- //donner/...\n", ubsan)
        pull_request = _read(".github/workflows/sanitizers-pr.yml")
        self.assertIn("run: bazelisk test --config=asan-geode-pr //tools/ci:asan_geode_pr\n",
                      pull_request)

    def _assert_rejected(self, check, cases):
        """Each weakened file must differ from the checked-in one and fail `check` for its reason.

        `cases` maps a name to (path, weakened text, expected message fragment); `check`
        receives every supply-chain file with that one replaced, plus the path, and returns the
        violations it finds.
        """
        files = self.supply_chain_files
        for name, (path, weakened, expected) in cases.items():
            with self.subTest(name=name):
                self.assertTrue(weakened != files[path], "mutation no longer applies")
                violations = [str(item) for item in check({**files, path: weakened}, path)]
                self.assertTrue(any(expected in item for item in violations),
                                "expected %r in %s" % (expected, violations))

    def test_every_workflow_parses_into_jobs(self):
        workflows = _workflows(self.supply_chain_files)
        self.assertGreaterEqual(len(workflows), 16)
        for path, workflow in sorted(workflows.items()):
            with self.subTest(path=path):
                self.assertTrue(_jobs(workflow), "no jobs parsed")

    def test_workflow_parser_fails_closed_on_layouts_it_cannot_read(self):
        commented = "jobs:\n  first:\n    runs-on: ubuntu-24.04\n# note\n  second:\n" \
                    "    runs-on: [self-hosted]\n"
        self.assertEqual({"first", "second"}, set(_jobs(commented)))
        for layout in ("jobs:\n    build:\n        runs-on: ubuntu-24.04\n",
                       "jobs:\n  build:\n   runs-on: ubuntu-24.04\n"):
            with self.subTest(layout=layout):
                with self.assertRaises(AssertionError):
                    _jobs(layout)

    def test_every_workflow_job_states_least_privilege_token_scopes(self):
        workflows = _workflows(self.supply_chain_files)
        for path, workflow in sorted(workflows.items()):
            with self.subTest(path=path):
                self.assertEqual([], _token_scope_violations(path, workflow))
        for path, job in WRITE_SCOPES:
            self.assertIn(job, _jobs(workflows[path]), "%s:%s" % (path, job))

    def test_token_scope_policy_rejects_widened_or_missing_permissions(self):
        files = self.supply_chain_files
        badges = ".github/workflows/badges.yaml"
        release = ".github/workflows/release.yml"
        publish = "    permissions:\n      contents: write\n      attestations: read\n"
        top = "permissions:\n  contents: read\n"
        self._assert_rejected(lambda files, path: _token_scope_violations(path, files[path]), {
            "workflow write-all": (badges, files[badges].replace(
                top, "permissions: write-all\n", 1), "more than read scopes to every job"),
            "workflow write": (badges, files[badges].replace(
                top, "permissions:\n  contents: write\n", 1), "more than read scopes to every job"),
            "missing": (badges, files[badges].replace(top, "", 1), "default token"),
            "extra job write": (release, files[release].replace(
                publish, publish + "      packages: write\n", 1), "'packages'"),
            "job write-all": (release, files[release].replace(
                publish, "    permissions: write-all\n", 1), "requests write scopes"),
        })

    def test_no_workflow_runs_privileged_triggers_for_untrusted_events(self):
        triggers = {path: _triggers(workflow)
                    for path, workflow in _workflows(self.supply_chain_files).items()}
        for path, events in sorted(triggers.items()):
            with self.subTest(path=path):
                self.assertTrue(events)
                self.assertEqual([], _privileged_trigger_violations(
                    path, self.supply_chain_files[path]))
        self.assertEqual(RUNNER_LOSS_TRIGGERS, triggers[RUNNER_LOSS_WORKFLOW])
        self.assertEqual({"release"}, triggers[".github/workflows/release.yml"])
        self.assertEqual({"workflow_dispatch"}, triggers[".github/workflows/publish_bcr.yml"])
        self.assertEqual({"workflow_dispatch"}, triggers[CANDIDATE_WORKFLOW])
        sanitizers = ".github/workflows/sanitizers.yml"
        dispatch = "  workflow_dispatch:\n"
        self._assert_rejected(_privileged_trigger_check, {
            "workflow_run": (sanitizers, self.sanitizers.replace(
                dispatch, dispatch + "  workflow_run:\n    workflows: [CI]\n", 1), "workflow_run"),
            "inline target": (sanitizers, re.sub(
                r"(?ms)^on:\n.*?^(?=\S)", "on: [push, pull_request_target]\n", self.sanitizers,
                count=1), "pull_request_target"),
            "issues": (sanitizers, self.sanitizers.replace(
                dispatch, dispatch + "  issues:\n", 1), "issues"),
        })

    def test_runner_loss_rerun_policy_rejects_widened_workflow_run_consumers(self):
        path = RUNNER_LOSS_WORKFLOW
        workflow = self.supply_chain_files[path]
        checkout = "          sparse-checkout: %s\n" % RUNNER_LOSS_SCRIPT
        rerun_command = RUNNER_LOSS_COMMANDS["rerun"]
        self._assert_rejected(_privileged_trigger_check, {
            "head checkout": (path, workflow.replace(
                checkout, checkout + "          ref: ${{ github.event.workflow_run.head_sha }}\n",
                1), "head_sha"),
            "whole checkout": (path, workflow.replace(checkout, "", 1), "more than the classifier"),
            "extra command": (path, workflow.replace(
                rerun_command, rerun_command + " && bash tools/other.sh", 1),
                "unreviewed commands"),
            "extra action": (path, workflow.replace(
                "      - name: Classify failed jobs and rerun runner losses\n",
                "      - uses: actions/setup-python@v6\n"
                "      - name: Classify failed jobs and rerun runner losses\n", 1),
                "actions other than one checkout"),
            "any branch": (path, workflow.replace(
                "      github.event.workflow_run.head_branch == 'main' &&\n", "", 1),
                "head_branch == 'main'"),
            "pull request runs": (path, workflow.replace(
                "      github.event.workflow_run.event == 'push' &&\n", "", 1), "'push'"),
            "repeat attempts": (path, workflow.replace(
                "      github.event.workflow_run.run_attempt == 1 }}",
                "      github.event.workflow_run.run_attempt >= 1 }}", 1), "gate is not exactly"),
            "widened gate": (path, workflow.replace(
                "      github.event.workflow_run.run_attempt == 1 }}",
                "      github.event.workflow_run.run_attempt == 1 || true }}", 1),
                "gate is not exactly"),
            "dispatch reaches the write job": (path, workflow.replace(
                "${{ github.event_name == 'workflow_run' &&",
                "${{ github.event_name == 'workflow_dispatch' || "
                "github.event_name == 'workflow_run' &&",
                1), "gate is not exactly"),
            "dry run on workflow_run": (path, workflow.replace(
                "if: ${{ github.event_name == 'workflow_dispatch' }}",
                "if: ${{ github.event_name == 'workflow_dispatch' || "
                "github.event_name == 'workflow_run' }}", 1), "gate is not exactly"),
            "extra step env": (path, workflow.replace(
                "          RUN_ID: ${{ github.event.workflow_run.id }}\n",
                "          RUN_ID: ${{ github.event.workflow_run.id }}\n"
                "          PYTHONPATH: ${{ github.event.workflow_run.display_title }}\n", 1),
                "step environment"),
            "job env": (path, workflow.replace(
                "    timeout-minutes: 5\n    permissions:\n      actions: write\n",
                "    timeout-minutes: 5\n    env:\n      PYTHONPATH: tools\n"
                "    permissions:\n      actions: write\n", 1), "step environment"),
            "workflow env": (path, workflow.replace(
                '  ASPECT_TOOLS_TELEMETRY: "-all"\n',
                '  ASPECT_TOOLS_TELEMETRY: "-all"\n  PYTHONSTARTUP: tools/x.py\n', 1),
                "workflow environment changed"),
            "branch filter": (path, workflow.replace(
                "    branches: [main]\n", "    branches: ['**']\n", 1), "filter changed"),
            "other workflows": (path, workflow.replace(
                'workflows: ["CI", "Editor WASM"]', 'workflows: ["CI", "Perf"]', 1),
                "filter changed"),
            "event text run ID": (path, workflow.replace(
                "RUN_ID: ${{ github.event.workflow_run.id }}",
                "RUN_ID: ${{ github.event.workflow_run.display_title }}", 1),
                "step environment"),
            "secret": (path, workflow.replace(
                "GH_TOKEN: ${{ github.token }}", "GH_TOKEN: ${{ secrets.PUBLISH_TOKEN }}", 1),
                "secrets."),
            "new trigger": (path, workflow.replace(
                "  workflow_dispatch:\n", "  workflow_dispatch:\n  issue_comment:\n", 1),
                "issue_comment"),
            "dispatch removed": (path, workflow.replace(
                "  workflow_dispatch:\n", "  push:\n", 1), "triggers changed"),
        })

    def test_each_secret_is_read_only_by_its_reviewed_step(self):
        self.assertEqual([], _secret_use_violations(_workflows(self.supply_chain_files)))

    def test_secret_policy_rejects_new_or_widened_secret_reads(self):
        files = self.supply_chain_files
        main = ".github/workflows/main.yml"
        badges = ".github/workflows/badges.yaml"
        step = "      - name: Upload CI diagnostics\n"
        runner = "    runs-on: ubuntu-latest\n"
        self._assert_rejected(lambda files, path: _secret_use_violations(_workflows(files)), {
            "new reader": (main, files[main].replace(
                step, step + "        env:\n          TOKEN: ${{ secrets.CODECOV_TOKEN }}\n", 1),
                "secret uses changed"),
            "job-wide env": (badges, files[badges].replace(
                runner, runner + "    env:\n      AUTH: ${{ secrets.GIST_SECRET }}\n", 1),
                "beyond one step"),
            "every secret": (badges, files[badges].replace(
                "${{ secrets.GIST_SECRET }}", "${{ toJSON(secrets) }}", 1),
                "forwards every secret"),
            "inherit": (main, files[main] + "  reuse:\n    uses: ./.github/workflows/lint.yml\n"
                        "    secrets: inherit\n", "forwards every secret"),
        })

    def test_reusable_workflow_calls_are_reviewed(self):
        self.assertEqual([], _workflow_call_violations(_workflows(self.supply_chain_files)))

    def test_workflow_call_policy_rejects_unreviewed_calls_and_secrets(self):
        files = self.supply_chain_files
        caller = ".github/workflows/pr_base_change.yml"
        main = ".github/workflows/main.yml"
        forward = ("      BUILDBUDDY_API_KEY_READONLY: "
                   "${{ secrets.BUILDBUDDY_API_KEY_READONLY }}\n")
        declared = "      BUILDBUDDY_API_KEY_READONLY:\n        required: false\n"
        self.assertIn(forward, files[caller])
        self.assertIn(declared, files[main])
        cases = {
            "new call": (main, files[main] + "  reuse:\n    uses: ./.github/workflows/lint.yml\n",
                         "reusable workflow calls changed"),
            "other callee": (caller, files[caller].replace(
                "uses: ./.github/workflows/main.yml", "uses: ./.github/workflows/lint.yml", 1),
                "calls ./.github/workflows/lint.yml"),
            "extra secret": (caller, files[caller].replace(
                forward, forward + "      CODECOV_TOKEN: ${{ secrets.CODECOV_TOKEN }}\n", 1),
                "forwards secrets other than"),
            "renamed secret": (caller, files[caller].replace(
                forward, "      BUILDBUDDY_API_KEY_READONLY: ${{ secrets.CODECOV_TOKEN }}\n", 1),
                "forwards secrets other than"),
            "undeclared in callee": (main, files[main].replace(declared, "", 1),
                                     "callee declares workflow_call secrets"),
        }
        self._assert_rejected(lambda files, path: _workflow_call_violations(_workflows(files)),
                              cases)
        # A forwarded secret outside the reviewed list also reads a secret beyond one step.
        self._assert_rejected(lambda files, path: _secret_use_violations(_workflows(files)), {
            "extra secret": cases["extra secret"][:2] + ("beyond one step",),
            "renamed secret": cases["renamed secret"][:2] + ("beyond one step",),
        })

    def test_whole_secret_objects_cannot_bypass_step_consumers(self):
        files = self.supply_chain_files
        path = ".github/workflows/main.yml"
        step = "      - name: Upload CI diagnostics\n"
        for reference in ("secrets", "toJSON ( secrets )", "ToJsOn\t( secrets )",
                          "format('{0}', secrets)"):
            with self.subTest(reference=reference):
                changed = files[path].replace(
                    step, step + "        env:\n          TOKEN: ${{ " + reference + " }}\n", 1)
                self.assertNotEqual(changed, files[path])
                self.assertTrue(_secret_use_violations(_workflows({**files, path: changed})))
        for literal in ("'secrets'", "'toJSON(secrets)'", "'secrets.UNREVIEWED_TOKEN'",
                        "'it''s secrets'", "'# secrets.UNREVIEWED_TOKEN'"):
            with self.subTest(literal=literal):
                self.assertEqual(set(), _secret_reads("${{ " + literal + " }}"))
                changed = files[path].replace(
                    step, step + "        env:\n          TOKEN: ${{ " + literal + " }}\n", 1)
                self.assertEqual([], _secret_use_violations(_workflows({**files, path: changed})))

    def test_quoted_expression_delimiters_do_not_hide_context_reads(self):
        files = self.supply_chain_files
        path = ".github/workflows/main.yml"
        step = "      - name: Upload CI diagnostics\n"
        for literal in ("'}}{0}'", "'it''s }}{0}'"):
            with self.subTest(literal=literal):
                expression = "format(" + literal + ", secrets.UNREVIEWED_TOKEN)"
                changed = files[path].replace(
                    step, step + "        env:\n          TOKEN: ${{ " + expression + " }}\n", 1)
                self.assertNotEqual(changed, files[path])
                with self.subTest(context="secret"):
                    self.assertTrue(_secret_use_violations(_workflows({**files, path: changed})))
                with self.subTest(context="event"):
                    self.assertTrue(_untrusted_script_contexts(
                        "      - run: echo ${{ format(" + literal +
                        ", github.event.pull_request.title) }}\n"))
        for literal in ("'github.event.pull_request.title'", "'}}github.event.pull_request.title'"):
            with self.subTest(safe_literal=literal):
                self.assertEqual([], _untrusted_script_contexts(
                    "      - run: echo ${{ " + literal + " }}\n"))

    def test_expression_extraction_terminates_and_keeps_adjacent_expressions(self):
        for expression in ("${{ secrets", "${{ 'unterminated }}", "${{ " + "'" * 4096):
            with self.subTest(malformed=expression[:30]):
                with self.assertRaisesRegex(ValueError, "unterminated expression marker"):
                    list(_expressions(expression))
        text = "${{ format('it''s }}{0}', secrets.GIST_SECRET) }} " \
               "${{ github.event.pull_request.title }}"
        expressions = list(_expressions(text))
        self.assertEqual(2, len(expressions))
        self.assertEqual(" format('it''s }}{0}', secrets.GIST_SECRET) ", expressions[0].group(1))
        self.assertEqual(" github.event.pull_request.title ", expressions[1].group(1))

    def test_unterminated_comment_marker_cannot_hide_a_later_secret_consumer(self):
        files = self.supply_chain_files
        path = ".github/workflows/badges.yaml"
        changed = files[path] + "\n      - name: Unreviewed consumer\n" \
                  "        # marker ${{ '\n" \
                  "        uses: example/consumer-action@v1.0.0\n" \
                  "        with:\n          auth: ${{ secrets.UNREVIEWED_TOKEN }}\n"
        self.assertNotEqual(changed, files[path])
        with self.assertRaisesRegex(ValueError, "unterminated expression marker"):
            _secret_use_violations(_workflows({**files, path: changed}))

    def test_comment_quote_pairs_cannot_hide_a_later_secret_consumer(self):
        files = self.supply_chain_files
        path = ".github/workflows/badges.yaml"
        for step in (
            "        uses: example/consumer-action@v1.0.0\n        # marker ${{ '\n",
            "        uses: example/consumer-action@v1.0.0 # marker ${{ '\n",
        ):
            with self.subTest(step=step):
                changed = files[path] + "\n      - name: Unreviewed consumer\n" + step + \
                          "        with:\n          auth: ${{ secrets.UNREVIEWED_TOKEN }}\n" \
                          "        # ' }}\n"
                self.assertNotEqual(changed, files[path])
                with self.assertRaisesRegex(ValueError, "ambiguous comment expression marker"):
                    _secret_use_violations(_workflows({**files, path: changed}))

    def test_only_gated_ci_jobs_reach_self_hosted_runners(self):
        workflows = _workflows(self.supply_chain_files)
        self.assertEqual([], _self_hosted_violations(workflows))
        # Matrix and reviewed-expression runners resolve to hosted images.
        hosted = {".github/workflows/bcr_preflight.yml": ("consumer", ["ubuntu-24.04", "xcode-27"]),
                  ".github/workflows/perf.yml": ("perf", ["xcode-27", "ubuntu-24.04"])}
        for path, (job, labels) in hosted.items():
            expected = [{label} for label in labels]
            with self.subTest(path=path):
                sets, problem = _runner_label_sets(path, job, _jobs(workflows[path])[job])
                self.assertIsNone(problem)
                self.assertEqual(expected, sets)

    def test_hosted_browser_subset_stays_hosted_and_never_writes_caches(self):
        # The trusted-PR hosted macOS browser subset reads the self-hosted routing output only to
        # run when the self-hosted macOS lane replaced the hosted one. It must stay on the hosted
        # image, and as a pull-request-only job it restores main's Actions caches without saving
        # any. Its BuildBuddy steps are the reviewed `macos` ones, checked by the BuildBuddy
        # policy above; no other step reads a secret.
        main = ".github/workflows/main.yml"
        workflow = _workflows(self.supply_chain_files)[main]
        job = _jobs(workflow)["macos-browser"]
        self.assertEqual("xcode-27", _job_key(job, "runs-on"))
        self.assertNotIn((main, "macos-browser"), SELF_HOSTED_JOBS)
        condition = _job_key(job, "if")
        self.assertIn("github.event_name == 'pull_request'", condition)
        self.assertIn("needs.gatekeeper.outputs.use_self_hosted_macos == 'true'", condition)
        self.assertIn("cache-save: false", job)
        self.assertNotRegex(job, r"uses: actions/cache(?:/save)?@")
        readers = {_step_key(step, "name", 8): _secret_reads(step) for step in _steps(job)
                   if _secret_reads(step)}
        self.assertEqual(readers, {
            "Configure BuildBuddy remote cache": {"BUILDBUDDY_API_KEY"},
            "Configure BuildBuddy read-only cache": {"BUILDBUDDY_API_KEY_READONLY"},
        })
        self.assertEqual(_secret_reads(job.split("    steps:\n", 1)[0]), set())

    def test_self_hosted_policy_rejects_ungated_or_mutable_runner_selection(self):
        files = self.supply_chain_files
        main = ".github/workflows/main.yml"
        coverage = ".github/workflows/coverage.yml"
        bcr = ".github/workflows/bcr_preflight.yml"
        perf = ".github/workflows/perf.yml"
        codeql = ".github/workflows/codeql.yml"
        # The full admission expression: the hosted browser subset job reads the same routing
        # output inside a different condition, and must not be the one this case mutates.
        # The hosted-parity job extends it with a reviewed narrowing condition.
        gate = SELF_HOSTED_GATE_PREFIX + "needs.gatekeeper.outputs.use_self_hosted_macos == 'true'"
        narrowing = SELF_HOSTED_NARROWING[(main, "macos-hosted-parity")]
        hosted = "    runs-on: ubuntu-24.04\n"
        ungated = "without the trusted gate"
        self.assertEqual(1, files[main].count(gate + " }}"))
        self.assertEqual(1, files[main].count(gate + narrowing + " }}"))
        self.assertEqual(1, files[bcr].count("        os: [ubuntu-24.04, xcode-27]\n"))
        self._assert_rejected(lambda files, path: _self_hosted_violations(_workflows(files)), {
            "ungated": (main, files[main].replace(gate + " }}", "true }}", 1), ungated),
            "parity narrowing widened": (main, files[main].replace(
                gate + narrowing, gate + narrowing.replace(" && (", " || (", 1), 1), ungated),
            "parity narrowing dropped": (main, files[main].replace(
                gate + narrowing, gate, 1), ungated),
            "status prefix widened": (main, files[main].replace(
                "${{ !cancelled() && needs.gatekeeper.outputs.is_duplicate_push",
                "${{ always() || needs.gatekeeper.outputs.is_duplicate_push", 1),
                ungated),
            "self-hosted label": (coverage, files[coverage].replace(
                hosted, "    runs-on: [self-hosted, linux, arm64]\n", 1), ungated),
            "custom labels only": (coverage, files[coverage].replace(
                hosted, "    runs-on: [linux, arm64]\n", 1), ungated),
            "custom scalar label": (coverage, files[coverage].replace(
                hosted, "    runs-on: arm64-builder\n", 1), ungated),
            "block list": (coverage, files[coverage].replace(
                hosted, "    runs-on:\n      - linux\n      - arm64\n", 1), ungated),
            "runner group": (coverage, files[coverage].replace(
                hosted, "    runs-on:\n      group: builders\n", 1), ungated),
            "variable runner": (coverage, files[coverage].replace(
                hosted, "    runs-on: ${{ vars.COVERAGE_RUNNER }}\n", 1), "mutable input"),
            "matrix label": (bcr, files[bcr].replace(
                "        os: [ubuntu-24.04, xcode-27]\n", "        os: [ubuntu-24.04, arm64]\n", 1),
                ungated),
            "matrix include label": (perf, files[perf].replace(
                "          - os: ubuntu-24.04\n", "          - os: linux-arm64\n", 1), ungated),
            "computed matrix": (bcr, files[bcr].replace(
                "        os: [ubuntu-24.04, xcode-27]\n", "        os: ${{ fromJSON(vars.OS) }}\n",
                1), "non-literal matrix"),
            "computed include beside exclusions": (bcr, files[bcr].replace(
                "        exclude:\n",
                "        include:\n          - os: ${{ vars.OS }}\n        exclude:\n", 1),
                "non-literal matrix"),
            "label after an exclusion": (bcr, files[bcr].replace(
                "'7.x' }}\n", "'7.x' }}\n        include:\n          - os: arm64\n", 1),
                ungated),
            "literal label in an exclusion": (bcr, files[bcr].replace(
                "'7.x' }}\n", "'7.x' }}\n          - os: arm64\n", 1), ungated),
            "anchored exclusion aliased into include": (bcr, files[bcr].replace(
                "'7.x' }}\n", "'7.x' }}\n          - &extra\n            os: arm64\n"
                "        include:\n          - *extra\n", 1), "non-literal matrix"),
            "flow mapping include": (bcr, files[bcr].replace(
                "'7.x' }}\n", "'7.x' }}\n        include: [{os: arm64}]\n", 1),
                "non-literal matrix"),
            "changed expression": (codeql, files[codeql].replace(
                "'ubuntu-latest' }}", "'arm64' }}", 1), "unreviewed expression"),
        })

    def test_artifacts_never_carry_raw_build_events_or_credential_jobs(self):
        files = self.supply_chain_files
        self.assertEqual([], _artifact_upload_violations(files))
        self.assertEqual(REVIEWED_UPLOAD_ACTIONS, _uploading_local_actions(files))
        # The self-hosted diagnostics keep their derived report, profiles and stall notes, and
        # each self-hosted job uploads only its derived coverage receipt besides them.
        main = _jobs(files[".github/workflows/main.yml"])
        uploads = _upload_paths(main["linux-self-hosted"])
        self.assertEqual(2, len(uploads))
        self.assertEqual(SELF_HOSTED_UPLOAD_FILES,
                         {entry.rsplit("/", 1)[-1] for entry in uploads[0] + uploads[1]})
        self.assertIn(["${{ runner.temp }}/coverage-receipt/coverage-receipt.json"], uploads)

    def test_upload_policy_rejects_raw_logs_directories_and_workspace_roots(self):
        files = self.supply_chain_files
        main = ".github/workflows/main.yml"
        coverage = ".github/workflows/coverage.yml"
        sanitizers = ".github/workflows/sanitizers.yml"
        wasm = ".github/workflows/editor_wasm.yml"
        report = "            ${{ runner.temp }}/donner-ci-diagnostics/report.md\n"
        summary = "${{ runner.temp }}/browser-gpu-serial-summary.json"
        evidence = "      - name: Retain failed test evidence\n"
        upload = ("      - name: Upload logs\n        uses: actions/upload-artifact@v7\n"
                  "        with:\n          name: logs\n          path: logs/test.log\n")
        cache = ("      - name: Save cache\n        uses: actions/cache/save@v6\n"
                 "        with:\n          key: k\n          path: build/bep.json\n")
        diagnostics = "      - name: Upload CI diagnostics\n"
        self._assert_rejected(lambda files, path: _artifact_upload_violations(files), {
            "self-hosted directory": (main, files[main].replace(
                report, "            ${{ runner.temp }}/donner-ci-diagnostics/\n", 1),
                "donner-ci-diagnostics/"),
            "self-hosted event log": (main, files[main].replace(
                report, report + report.replace("report.md", "*/bep.json"), 1), "bep.json"),
            "hosted event log": (main, files[main].replace(
                summary, "${{ runner.temp }}/browser-gpu-serial.bep.json", 1), "serial.bep.json"),
            "workspace root": (coverage, files[coverage].replace(
                "path: coverage-report/*.log", "path: .", 1), "uploads ."),
            "credential job workspace": (sanitizers, files[sanitizers].replace(
                evidence, upload.replace("logs/test.log", ".") + evidence, 1),
                "after writing a cache credential"),
            "credential job temp root": (sanitizers, files[sanitizers].replace(
                evidence, upload.replace("logs/test.log", "${{ runner.temp }}") + evidence, 1),
                "after writing a cache credential"),
            "credential directory": (main, files[main].replace(
                summary, "${{ runner.temp }}/buildbuddy/credentials.bazelrc", 1),
                "after writing a cache credential"),
            "reviewed output in another job": (main, files[main].replace(
                summary, "${{ steps.stage-package.outputs.handoff_dir }}", 1),
                "after writing a cache credential"),
            "reviewed output producer runs another command": (wasm, files[wasm].replace(
                "        run: python3 tools/ci/editor_wasm_stage.py package\n",
                "        run: |\n          python3 tools/ci/editor_wasm_stage.py package\n"
                '          echo "handoff_dir=${RUNNER_TEMP}" >> "${GITHUB_OUTPUT}"\n', 1),
                "after writing a cache credential"),
            "reviewed output producer command continued": (wasm, files[wasm].replace(
                "        run: python3 tools/ci/editor_wasm_stage.py package\n",
                "        run: python3 tools/ci/editor_wasm_stage.py package\n"
                "          && cp -R ${RUNNER_TEMP} .\n", 1),
                "after writing a cache credential"),
            "reviewed output from another script": (wasm, files[wasm].replace(
                "run: python3 tools/ci/editor_wasm_stage.py package",
                "run: python3 tools/ci/other_stage.py package", 1),
                "after writing a cache credential"),
            "credential glob": (main, files[main].replace(
                summary, "${{ runner.temp }}/**", 1), "after writing a cache credential"),
            "credential absolute temp": (main, files[main].replace(
                summary, "/Users/runner/work/_temp/*/credentials.bazelrc", 1),
                "after writing a cache credential"),
            "cache save event log": (coverage, files[coverage].replace(
                "      - name: Generate coverage proof\n",
                cache + "      - name: Generate coverage proof\n", 1), "build/bep.json"),
            "self-hosted cache save": (main, files[main].replace(
                diagnostics, "      - uses: actions/cache@v6\n" + diagnostics, 1),
                "saves an Actions cache"),
            "unreviewed upload action": (
                ".github/actions/apt-install/action.yml",
                files[".github/actions/apt-install/action.yml"]
                + "    - uses: actions/upload-artifact@v7\n", "unreviewed action"),
        })

    def test_reviewed_output_scripts_write_only_the_reviewed_values(self):
        files = self.supply_chain_files
        stage = _read(EDITOR_WASM_STAGE)
        handoff = 'handoff_dir = f"{runner_temp}/editor-wasm-package-geode"'
        write = '_write_output(environ, "handoff_dir", handoff_dir)'
        self.assertEqual(1, stage.count(handoff))
        self.assertEqual(1, stage.count(write))
        weakened = {
            "written twice": stage.replace(
                write, write + '\n    _write_output(environ, "handoff_dir", runner_temp)', 1),
            "written from another value": stage.replace(
                write, '_write_output(environ, "handoff_dir", runner_temp)', 1),
            "retargeted": stage.replace(handoff, 'handoff_dir = f"{runner_temp}"', 1),
            "reassigned": stage.replace(handoff, handoff + "\n    handoff_dir = runner_temp", 1),
            "temporary directory redefined": stage.replace(
                handoff, 'runner_temp = "/"\n    ' + handoff, 1),
            "candidate retargeted": stage.replace(
                "/donner-editor-wasm-geode-{environ['GITHUB_SHA']}", "", 1),
            "output name built at run time": stage.replace(
                write, write + '\n    _write_output(environ, "handoff" + "_dir", runner_temp)', 1),
            "output file written directly": stage.replace(
                write, write + '\n    with open(environ["GITHUB_OUTPUT"], "a") as output:'
                '\n        output.write(f"handoff_dir={runner_temp}\\n")', 1),
            "output file named indirectly": stage.replace(
                write, write + '\n    name = "GITHUB_OUTPUT"'
                '\n    Path(environ[name]).write_text(f"handoff_dir={runner_temp}\\n")', 1),
        }
        for name, text in weakened.items():
            with self.subTest(name=name):
                self.assertNotEqual(text, stage)
                violations = _artifact_upload_violations({**files, EDITOR_WASM_STAGE: text})
                self.assertTrue(any("after writing a cache credential" in item
                                    for item in violations), violations)

    def test_scripts_never_interpolate_untrusted_event_text(self):
        for path, text in sorted(self.supply_chain_files.items()):
            with self.subTest(path=path):
                self.assertEqual([], _untrusted_script_contexts(text))

    def test_script_context_policy_rejects_event_text_and_inputs(self):
        fixture = textwrap.dedent("""\
            steps:
              - run: echo "${{ github.event.pull_request.title }}"
              - name: Inline branch
                run: echo ${{ github.head_ref }}
              - uses: actions/github-script@v9
                with:
                  script: |
                    core.info(`${{ inputs.artifact_id }}`)
              - uses: nick-fields/retry@v4
                with:
                  command: >-
                    make ${{ github.event.comment.body }}
              - run: git diff ${{ github.event.pull_request.base.sha }}
              - run: git show ${{ github.event.pull_request.head.sha }}
            """)
        self.assertEqual(["github.event.pull_request.title", "github.head_ref", "inputs.",
                          "github.event.comment.body"], _untrusted_script_contexts(fixture))

    def test_ci_definitions_name_no_private_network_or_home_directory(self):
        for path, text in sorted(self.supply_chain_files.items()):
            with self.subTest(path=path):
                self.assertEqual([], PRIVATE_NETWORK_LITERAL.findall(text))
        expected = [
            ".".join(("172", "20", "1", "2")),
            "/".join(("", "home", "alice")),
            "/".join(("", "Users", "bob")),
            ".".join(("10", "1", "2", "3")),
            ".".join(("100", "64", "0", "1")),
            ":".join(("fd12", "3456", "", "1")),
            ".".join(("builder", "lan")),
            ".".join(("cache", "example", "internal")),
            ".".join(("nas", "home", "arpa")),
        ]
        fixture = ("http://{}:1234 {}/.cache {}/bin {} {} {} {} {} {} "
                   "/home/runner/work /home/linuxbrew/.linuxbrew /Users/runner/bin v10.1.2 "
                   "172.15.0.1 cache.example.invalid fc.example.com").format(*expected)
        self.assertEqual(expected, PRIVATE_NETWORK_LITERAL.findall(fixture))



    def test_secret_policy_handles_indexed_and_compound_references(self):
        files = self.supply_chain_files
        badges = ".github/workflows/badges.yaml"
        for reference in ("secrets['GIST_SECRET']", "secrets [ 'GIST_SECRET' ]",
                          "format('{0}', secrets['GIST_SECRET'])"):
            with self.subTest(reference=reference):
                changed = files[badges].replace("secrets.GIST_SECRET", reference)
                self.assertEqual([], _secret_use_violations(_workflows({**files, badges: changed})))
        main = ".github/workflows/main.yml"
        step = "      - name: Upload CI diagnostics\n"
        for reference in ("secrets['UNREVIEWED_TOKEN']", "secrets[inputs.token_name]",
                          "join(secrets.*, 'fixture')"):
            with self.subTest(reference=reference):
                changed = files[main].replace(
                    step, step + "        env:\n          TOKEN: ${{ " + reference + " }}\n", 1)
                self.assertTrue(any("secret uses changed" in item for item in
                                    _secret_use_violations(_workflows({**files, main: changed}))))

    def test_indexed_secret_cannot_move_to_job_or_workflow_scope(self):
        files = self.supply_chain_files
        badges = ".github/workflows/badges.yaml"
        for anchor, injection in (
            ("    runs-on: ubuntu-latest\n",
             "    env:\n      AUTH: ${{ secrets['GIST_SECRET'] }}\n"),
            ("jobs:\n", "env:\n  AUTH: ${{ secrets['GIST_SECRET'] }}\njobs:\n"),
        ):
            with self.subTest(anchor=anchor):
                replacement = injection if anchor == "jobs:\n" else anchor + injection
                changed = files[badges].replace(anchor, replacement, 1)
                self.assertNotEqual(changed, files[badges])
                self.assertTrue(any("beyond one step" in item for item in
                                    _secret_use_violations(_workflows({**files, badges: changed}))))

    def test_indexed_event_text_is_rejected_in_every_script_kind(self):
        for key in ("run", "script", "command"):
            for reference in ("github.event['pull_request']['title']",
                              "github['event']['pull_request'].body",
                              "github [ 'head_ref' ]", "inputs['message']",
                              "github.event['pull_request.head.sha']",
                              "github.event[inputs.event_key]"):
                with self.subTest(key=key, reference=reference):
                    script = "      - " + key + ": echo ${{ " + reference + " }}\n"
                    self.assertTrue(_untrusted_script_contexts(script))
        for reference in ("github.event['pull_request']['head']['sha']",
                          "github['event'].pull_request.number", "github.event_name"):
            with self.subTest(safe=reference):
                self.assertEqual([], _untrusted_script_contexts(
                    "      - run: echo ${{ " + reference + " }}\n"))



    def test_public_diagnostics_do_not_enable_raw_console_tails(self):
        summary = _step_body(self.supply_chain_files[".github/workflows/main.yml"],
                             "Summarize diagnostics")
        self.assertNotIn("--include-console-tails", summary)



    def test_secret_consumers_are_reviewed_at_each_step(self):
        files = self.supply_chain_files
        path = ".github/workflows/badges.yaml"
        added = files[path] + "      - name: Unreviewed consumer\n" \
                "        uses: example/consumer-action@v1.0.0\n" \
                "        with:\n          token: ${{ secrets.GIST_SECRET }}\n"
        self.assertTrue(_secret_use_violations(_workflows({**files, path: added})))
        reviewed = next(step for step in _steps(_jobs(files[path])["update-badges"])
                        if _secret_reads(step))
        duplicated = files[path] + "\n      - " + reviewed
        self.assertTrue(_secret_use_violations(_workflows({**files, path: duplicated})))
        changed_id = files[path].replace("      - name: Create Lines-of-Code-Badge\n",
                                        "      - name: Create Lines-of-Code-Badge\n"
                                        "        id: alternate-consumer\n", 1)
        self.assertTrue(_secret_use_violations(_workflows({**files, path: changed_id})))
        changed = files[path].replace("uses: schneegans/dynamic-badges-action@v1.9.0",
                                    "uses: example/consumer-action@v1.0.0", 1)
        self.assertTrue(_secret_use_violations(_workflows({**files, path: changed})))

    def test_self_hosted_gate_cannot_be_weakened_by_boolean_context(self):
        files = self.supply_chain_files
        path = ".github/workflows/main.yml"
        for runner in ("linux", "macos"):
            gate = "needs.gatekeeper.outputs.use_self_hosted_" + runner + " == 'true'"
            for replacement in ("true || " + gate, gate + " || true"):
                with self.subTest(runner=runner, replacement=replacement):
                    job = _jobs(files[path])[runner + "-self-hosted"]
                    weakened_job = job.replace(gate, replacement, 1)
                    self.assertNotEqual(weakened_job, job)
                    changed = files[path].replace(job, weakened_job, 1)
                    self.assertNotEqual(changed, files[path])
                    self.assertTrue(_self_hosted_violations(_workflows({**files, path: changed})))

    def test_hosted_runner_prefixes_do_not_approve_custom_labels(self):
        files = self.supply_chain_files
        path = ".github/workflows/coverage.yml"
        for label in ("ubuntu-unreviewed-worker", "macos-unreviewed-worker",
                      "windows-unreviewed-worker"):
            with self.subTest(label=label):
                changed = files[path].replace("runs-on: ubuntu-24.04", "runs-on: " + label, 1)
                self.assertNotEqual(changed, files[path])
                self.assertTrue(_self_hosted_violations(_workflows({**files, path: changed})))

    def test_reviewed_collector_must_upload_only_its_collected_tree(self):
        files = self.supply_chain_files
        path = ".github/actions/upload-bazel-test-artifacts/action.yml"
        for upload in (".", "${{ github.workspace }}", "${{ runner.temp }}/other-tree"):
            with self.subTest(upload=upload):
                changed = files[path].replace("${{ runner.temp }}/bazel-test-artifacts", upload)
                self.assertNotEqual(changed, files[path])
                self.assertTrue(_artifact_upload_violations({**files, path: changed}))


if __name__ == "__main__":
    unittest.main()
