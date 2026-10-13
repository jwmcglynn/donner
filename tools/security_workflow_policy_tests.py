"""Pins review-driven security workflow selection and scheduling policy."""

from collections import Counter
import os
from pathlib import Path
import re
import subprocess
import tempfile
import textwrap
import unittest

from python.runfiles import runfiles


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
    (".github/workflows/github_hosted_nightly.yml", "build", "Configure BuildBuddy remote cache", None,
     "./.github/actions/buildbuddy-cache"): {"BUILDBUDDY_API_KEY"},
    (".github/workflows/main.yml", "macos-self-hosted", "Configure masked remote cache", None,
     "<run>"): {"BAZEL_REMOTE_CACHE"},
    (".github/workflows/publish_bcr.yml", "publish-fork", "Create the fork branch only if absent", None,
     "<run>"): {"PUBLISH_TOKEN"},
    (".github/workflows/sanitizers.yml", "asan", "Configure BuildBuddy remote cache", None,
     "./.github/actions/buildbuddy-cache"): {"BUILDBUDDY_API_KEY"},
    (".github/workflows/sanitizers.yml", "ubsan", "Configure BuildBuddy remote cache", None,
     "./.github/actions/buildbuddy-cache"): {"BUILDBUDDY_API_KEY"},
}

# The only jobs that may run on self-hosted runners, with the gatekeeper output admitting each.
SELF_HOSTED_JOBS = {
    (".github/workflows/main.yml", "linux-self-hosted"): "use_self_hosted_linux",
    (".github/workflows/main.yml", "macos-self-hosted"): "use_self_hosted_macos",
}

# A runner label set is GitHub-hosted only when it is exactly one hosted image label. Any other
# label, label list or runner group can route a job to a self-hosted runner.
HOSTED_IMAGES = frozenset({"ubuntu-24.04", "ubuntu-latest", "macos-26", "macos-latest"})

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

# Files a self-hosted job may upload directly. Raw Bazel event streams and execution logs record
# the runner's effective configuration, so they never leave the runner.
SELF_HOSTED_UPLOAD_FILES = {"manifest.txt", "profile.gz", "report.md", "stall.txt",
                            "target-list.txt"}

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


def _secret_use_violations(workflows):
    records = []
    violations = []
    for path, workflow in workflows.items():
        if (re.search(r"secrets:\s*inherit", workflow, re.IGNORECASE) or
                "<whole secret object>" in _secret_reads(workflow)):
            violations.append("%s forwards every secret" % path)
        if _unscoped_secret_reads(workflow):
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
    gate = SELF_HOSTED_GATE_PREFIX + "needs.gatekeeper.outputs.%s == 'true' }}" % \
        SELF_HOSTED_JOBS.get((path, name))
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


def _upload_violations(path, name, job, uploading_actions):
    violations = []
    local = set(re.findall(r"uses: (\./\.github/actions/[A-Za-z0-9_-]+)", job))
    # The reviewed collector copies only Bazel test logs, never the workspace that holds the
    # generated credential fragment, so it stays usable in jobs that write one.
    uploads = _upload_paths(job) or local & uploading_actions - REVIEWED_UPLOAD_ACTIONS
    if "./.github/actions/buildbuddy-cache" in job and uploads:
        violations.append("%s:%s uploads files after writing a cache credential" % (path, name))
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
        for violation in _upload_violations(path, name, job, uploading_actions)]


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
                timeout=10,
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
        self.assertIn("- os: macos-26", job)
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
        body = action.split("      run:", 1)[1].split("\n\n", 1)[0]
        script = textwrap.dedent(body.split("\n", 1)[1]) if body.strip().startswith("|") else body.strip()
        with tempfile.TemporaryDirectory() as directory:
            rc = Path(directory) / ".bazelrc"
            original = "common --repository_cache=/fixture/repository"
            rc.write_text(original)
            result = subprocess.run(["/bin/bash", "-c", script], env={"HOME": directory},
                                    text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            lines = rc.read_text().splitlines()
            self.assertEqual(lines[0], original)
            self.assertEqual(lines[1], "build --disk_cache=~/.cache/bazel-disk")

    def test_compiled_action_cache_refreshes_only_from_main(self):
        action = self.supply_chain_files[".github/actions/cache-bazel-actions/action.yml"]
        writer = action.split("    - name: Restore and refresh main action cache\n", 1)[1]
        writer, reader = writer.split("    - name: Restore action cache without saving\n", 1)
        self.assertIn("github.event_name != 'pull_request' && github.ref == 'refs/heads/main'", writer)
        self.assertIn("uses: actions/cache@v6", writer)
        self.assertIn("github.event_name == 'pull_request' || github.ref != 'refs/heads/main'", reader)
        self.assertIn("uses: actions/cache/restore@v6", reader)
        for block in (writer, reader):
            key = re.search(r"^        key: (.+)$", block, re.MULTILINE).group(1)
            for dimension in ("runner.os", "runner.arch", "inputs.namespace", "github.sha"):
                self.assertIn(dimension, key)
            self.assertIn("hashFiles('.bazelversion')", key)
            restore = block.split("restore-keys: |", 1)[1]
            self.assertNotIn("github.sha", restore)
            self.assertIn("inputs.namespace", restore)
        self.assertNotIn("secrets.", action)

    def test_self_hosted_cache_endpoint_is_absent_from_job_environment(self):
        workflow = self.supply_chain_files[".github/workflows/main.yml"]
        hosted, self_hosted = workflow.split("\n  macos-self-hosted:\n", 1)
        header = self_hosted.split("    steps:\n", 1)[0]
        self.assertNotIn("--remote_cache=", header)
        self.assertNotIn("vars.BAZEL_REMOTE_CACHE", workflow)
        self.assertNotIn("secrets.BAZEL_REMOTE_CACHE", hosted)
        mask = _step_body(self_hosted, "Configure masked remote cache")
        self.assertIn("REMOTE_CACHE_URL: ${{ secrets.BAZEL_REMOTE_CACHE }}", mask)
        self.assertLess(self_hosted.index("- name: Configure masked remote cache"),
                        self_hosted.index("- name: Check runner disk space"))

    def test_remote_cache_setup_masks_values_before_export_and_fails_closed(self):
        workflow = self.supply_chain_files[".github/workflows/main.yml"]
        body = _step_body(workflow, "Configure masked remote cache")
        scripts = _run_bodies(body)
        self.assertEqual(len(scripts), 1)
        endpoint = "https://cache.example.invalid:9443/fixture"
        with tempfile.TemporaryDirectory() as directory:
            environment = Path(directory) / "environment"
            environment.write_text("EXISTING=preserved")
            result = subprocess.run(["/bin/bash", "-c", textwrap.dedent(scripts[0])],
                                    env={**os.environ, "REMOTE_CACHE_URL": endpoint,
                                         "GITHUB_ENV": str(environment),
                                         "BAZEL_MACOS_BUILD_FLAGS": "--macos_minimum_os=13.3"},
                                    text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            commands = result.stdout.splitlines()
            self.assertTrue(commands)
            self.assertTrue(all(line.startswith("::add-mask::") for line in commands))
            for value in (endpoint, "cache.example.invalid", "cache.example.invalid:9443", "/fixture"):
                self.assertIn("::add-mask::" + value, commands)
            self.assertEqual(environment.read_text().splitlines(), [
                "EXISTING=preserved",
                "BAZEL_MACOS_BUILD_FLAGS=--macos_minimum_os=13.3 --remote_cache=" + endpoint,
            ])
            for invalid in ("", "invalid-cache-value", "https://cache.example.invalid/\ninjected",
                            "https://fixture-user:fixture-token@cache.example.invalid",
                            "https://cache.example.invalid?token=fixture-token",
                            "https://cache.example.invalid#fixture-token"):
                environment.write_text("EXISTING=preserved")
                result = subprocess.run(["/bin/bash", "-c", textwrap.dedent(scripts[0])],
                                        env={**os.environ, "REMOTE_CACHE_URL": invalid,
                                             "GITHUB_ENV": str(environment),
                                             "BAZEL_MACOS_BUILD_FLAGS": "--macos_minimum_os=13.3"},
                                        text=True, capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(environment.read_text(), "EXISTING=preserved")
                self.assertEqual(result.stdout, "")
                if invalid:
                    self.assertNotIn(invalid, result.stderr)

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
        cases = (
            (".github/workflows/badges.yaml", "Install cloc", CLOC_SHA256),
            (".github/workflows/deploy_docs.yaml", "Install Doxygen", DOXYGEN_SHA256),
        )
        for path, step, digest in cases:
            with self.subTest(path=path):
                body = _step_body(self.supply_chain_files[path], step)
                self.assertIn(digest, body)
                verification = "sha256sum --check --strict"
                self.assertIn(verification, body)
                self.assertLess(body.index(verification), body.index("tar "))

    def test_documentation_publication_requires_a_manual_request(self):
        workflow = self.supply_chain_files[".github/workflows/deploy_docs.yaml"]
        deploy = workflow.split("\n  deploy:\n", 1)[1]
        self.assertIn("if: github.event_name == 'workflow_dispatch'", deploy)
        self.assertIn("branches: [\"main\"]", workflow)

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
        )
        for path, action, name in uploads:
            with self.subTest(path=path, name=name):
                steps = [step for step in self.supply_chain_files[path].split("\n      - ")
                         if action in step and name in step]
                self.assertEqual(len(steps), 1)
                self.assertRegex(steps[0], r"\n          retention-days: 90\n")

    def test_release_only_verifies_retained_preflight_bytes_before_publication(self):
        release = self.supply_chain_files[".github/workflows/release.yml"]
        self.assertIn("types: [published]", release)
        self.assertNotIn("edited", release)
        self.assertNotIn("workflow_dispatch", release)
        self.assertIn("permissions:\n  contents: read", release)
        self.assertIn("publish-release-artifacts:", release)
        self.assertIn("contents: write", release)
        self.assertIn("attestations: read", release)
        self.assertNotIn("id-token: write", release)
        self.assertIn("actions/download-artifact@", release)
        self.assertNotIn("actions/attest-build-provenance@", release)
        # One loop verifies the preflight attestation of every attested release subject.
        self.assertEqual(release.count("gh attestation verify"), 1)
        self.assertEqual(release.count("--signer-workflow jwmcglynn/donner/.github/workflows/bcr_preflight.yml"), 1)
        attested = release.split("          for subject in \\\n", 1)[1].split("; do\n", 1)[0]
        cli_assets = [
            f"release/{directory}/donner-svg_{platform}{suffix}"
            for directory, platform in (("linux", "linux_x86_64"), ("macos", "darwin_arm64"))
            for suffix in ("", ".bazel.lock", ".NOTICE.txt")
        ]
        self.assertEqual(
            [line.strip().rstrip(" \\") for line in attested.splitlines()],
            cli_assets + ["release/source/*.tar.gz"],
        )
        self.assertNotIn("bazelisk build", release)
        self.assertNotIn("actions/upload-artifact@", release)
        self.assertNotIn("--lockfile_mode=off", release)
        self.assertNotIn("tools.bcr_release plan-binaries", release)
        publication = release.split("  publish-release-artifacts:", 1)[1]
        self.assertIn("needs: resolve-source", publication)
        self.assertIn("tools.bcr_source verify", publication)
        self.assertEqual(publication.count("tools.release_cli verify"), 2)
        self.assertEqual(publication.count("run-id: ${{ needs.resolve-source.outputs.run_id }}"), 3)
        self.assertIn("release/source/*", publication)
        self.assertIn("RELEASE_UPLOAD_URL: ${{ github.event.release.upload_url }}", release)
        self.assertIn("Upload verified release artifacts without replacement", release)
        self.assertIn("python3 tools/release_artifact_publisher.py", release)
        uploaded = release.split("python3 tools/release_artifact_publisher.py", 1)[1]
        for asset in cli_assets:
            self.assertIn(f"            {asset} \\\n", uploaded)
        self.assertNotIn("softprops/action-gh-release", release)
        self.assertNotIn("publish-to-bcr", release)
        self.assertNotIn("svenstaro/upload-release-action", release)
        for body in _run_bodies(release):
            self.assertNotIn("${{", body, "release run blocks must use quoted environment values")

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
        self.assertIn("os: [ubuntu-24.04, macos-26]", consumer)
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
                timeout=60)
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
        self.assertIn("        os: [ubuntu-24.04, macos-26]\n", jobs["consumer"])
        self.assertIn("        bazel: ['7.x', '8.x']\n", jobs["consumer"])
        exclusions = _indented_block(
            jobs["consumer"].split("        exclude:\n", 1)[1].splitlines(), 8)
        self.assertEqual(["          - os: ${{ %s && 'none' || 'macos-26' }}" % full,
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
        self.assertIn("approved_source_commit:", workflow)
        self.assertIn("approved_source_sha256:", workflow)
        self.assertIn("github.ref == 'refs/heads/main'", workflow)
        self.assertIn("ref: ${{ github.sha }}", workflow)
        self.assertIn("tools.bcr_release plan-submission", workflow)
        self.assertIn("--approved-commit", workflow)
        self.assertIn("--approved-sha256", workflow)
        validate, remainder = workflow.split("\n  generate-entry:\n", 1)
        generate, remainder = remainder.split("\n  prepare-fork:\n", 1)
        prepare, publish = remainder.split("\n  publish-fork:\n", 1)
        self.assertNotIn("secrets.", validate)
        self.assertNotIn("secrets.", generate)
        # Any release tag is accepted, so Renovate's tag updates stay green; a commit pin is not.
        self.assertRegex(generate, r"uses: bazel-contrib/publish-to-bcr@v\d+\.\d+\.\d+\n")
        self.assertIn("actions/upload-artifact@v7", generate)
        self.assertIn("actions/download-artifact@v8", prepare)
        self.assertEqual(workflow.count("actions/checkout@v7"), 7)
        self.assertNotRegex(workflow, r"uses: [^\n]+@[0-9a-f]{40}(?:\s|$)")
        self.assertIn("tools.bcr_release verify-generated", generate)
        self.assertIn("git -C registry bundle create", generate)
        self.assertIn("actions/upload-artifact@", generate)
        self.assertIn("fetch-depth: 0", _step_body(generate, "Checkout BCR for the proposed entry"))
        self.assertIn("needs.validate.outputs.prepare == 'true'", prepare)
        self.assertIn("needs: [validate, generate-entry]", prepare)
        self.assertNotIn("secrets.", prepare)
        self.assertIn("tools.bcr_release verify-generated", prepare)
        self.assertIn("actions/download-artifact@", prepare)
        self.assertIn("fetch-depth: 0", _step_body(prepare, "Checkout the exact BCR base"))
        self.assertIn("needs: [validate, generate-entry, prepare-fork]", publish)
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

    def test_pull_requests_never_receive_the_buildbuddy_write_key(self):
        guarded = (
            "github.ref == 'refs/heads/main' && "
            "github.event_name != 'pull_request' && "
            "github.event_name != 'pull_request_target'"
        )
        self.assertNotIn(
            "secrets.BUILDBUDDY_API_KEY",
            self.supply_chain_files[".github/workflows/sanitizers-pr.yml"],
        )
        for path, workflow in self.supply_chain_files.items():
            if re.search(r"(?m)^  pull_request:", workflow):
                with self.subTest(path=path, invariant="no-pr-cache-secret"):
                    self.assertNotIn("secrets.BUILDBUDDY_API_KEY", workflow)
        for path, workflow in self.supply_chain_files.items():
            if "uses: ./.github/actions/buildbuddy-cache" not in workflow:
                continue
            bodies = workflow.split("      - name: Configure BuildBuddy remote cache\n")[1:]
            for index, body in enumerate(bodies):
                step = body.split("\n      - name:", 1)[0]
                with self.subTest(path=path, step=index):
                    self.assertIn(guarded, step)
                    self.assertIn("secrets.BUILDBUDDY_API_KEY", step)

    def test_sensitive_workflows_use_least_privilege_and_drop_checkout_credentials(self):
        paths = (
            ".github/workflows/badges.yaml",
            ".github/workflows/cmake.yml",
            ".github/workflows/lint.yml",
            ".github/workflows/release.yml",
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
                                    capture_output=True, text=True, timeout=10)
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
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(17, result.returncode, result.stdout + result.stderr)
            self.assertEqual(1, receipt.read_text().count("\0"))
        self.assertNotRegex(body.replace("//...", ""), r"//[^\s]+")

    def test_fuzz_variants_are_selected_by_bazel_tags(self):
        expected = {
            "Test text-full fuzzers": "fuzz_text_full",
            "Test Geode render fuzzers": "fuzz_geode",
        }
        for step, tag in expected.items():
            body = _step_body(self.fuzz, step)
            self.assertIn("--test_tag_filters=%s" % tag, body)
            self.assertIn("--build_tag_filters=%s" % tag, body)
            self.assertNotRegex(body.replace("//...", ""), r"//[^\s]+")

    def test_production_image_decode_soak_runs_on_both_nightly_hosts(self):
        target = "//tools/ci:image_loader_production_soak"
        self.assertEqual(self.fuzz.count("Soak image loader at production decode budget"), 2)
        self.assertEqual(self.fuzz.count(target), 2)
        for job in ("linux", "macos"):
            section = self.fuzz.split("\n  %s:\n" % job, 1)[1]
            if job == "linux":
                section = section.split("\n  macos:\n", 1)[0]
            self.assertIn("--config=asan-fuzzer", section)
            self.assertIn("--config=fuzz-soak", section)

    def test_ubsan_corpora_are_selected_by_bazel_tags(self):
        body = _step_body(self.sanitizers, "Replay untrusted-input fuzzer corpora with UBSan")
        for tag in ("fuzz_ubsan", "fuzz_ubsan_text_full", "fuzz_ubsan_geode"):
            self.assertIn("--test_tag_filters=%s" % tag, body)
            self.assertIn("--build_tag_filters=%s" % tag, body)
        self.assertNotRegex(body.replace("//...", ""), r"//[^\s]+")

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
                self.assertEqual(set(), events & PRIVILEGED_TRIGGERS)
        self.assertEqual({"release"}, triggers[".github/workflows/release.yml"])
        self.assertEqual({"workflow_dispatch"}, triggers[".github/workflows/publish_bcr.yml"])
        sanitizers = ".github/workflows/sanitizers.yml"
        dispatch = "  workflow_dispatch:\n"
        self._assert_rejected(lambda files, path: _triggers(files[path]) & PRIVILEGED_TRIGGERS, {
            "workflow_run": (sanitizers, self.sanitizers.replace(
                dispatch, dispatch + "  workflow_run:\n    workflows: [CI]\n", 1), "workflow_run"),
            "inline target": (sanitizers, re.sub(
                r"(?ms)^on:\n.*?^(?=\S)", "on: [push, pull_request_target]\n", self.sanitizers,
                count=1), "pull_request_target"),
            "issues": (sanitizers, self.sanitizers.replace(
                dispatch, dispatch + "  issues:\n", 1), "issues"),
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
        hosted = {".github/workflows/bcr_preflight.yml": ("consumer", ["ubuntu-24.04", "macos-26"]),
                  ".github/workflows/perf.yml": ("perf", ["macos-26", "ubuntu-24.04"])}
        for path, (job, labels) in hosted.items():
            expected = [{label} for label in labels]
            with self.subTest(path=path):
                sets, problem = _runner_label_sets(path, job, _jobs(workflows[path])[job])
                self.assertIsNone(problem)
                self.assertEqual(expected, sets)

    def test_self_hosted_policy_rejects_ungated_or_mutable_runner_selection(self):
        files = self.supply_chain_files
        main = ".github/workflows/main.yml"
        coverage = ".github/workflows/coverage.yml"
        bcr = ".github/workflows/bcr_preflight.yml"
        perf = ".github/workflows/perf.yml"
        codeql = ".github/workflows/codeql.yml"
        gate = "needs.gatekeeper.outputs.use_self_hosted_macos == 'true'"
        hosted = "    runs-on: ubuntu-24.04\n"
        ungated = "without the trusted gate"
        self.assertEqual(1, files[main].count(gate))
        self.assertEqual(1, files[bcr].count("        os: [ubuntu-24.04, macos-26]\n"))
        self._assert_rejected(lambda files, path: _self_hosted_violations(_workflows(files)), {
            "ungated": (main, files[main].replace(gate, "true", 1), ungated),
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
                "        os: [ubuntu-24.04, macos-26]\n", "        os: [ubuntu-24.04, arm64]\n", 1),
                ungated),
            "matrix include label": (perf, files[perf].replace(
                "          - os: ubuntu-24.04\n", "          - os: linux-arm64\n", 1), ungated),
            "computed matrix": (bcr, files[bcr].replace(
                "        os: [ubuntu-24.04, macos-26]\n", "        os: ${{ fromJSON(vars.OS) }}\n",
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
        # The self-hosted diagnostics keep their derived report, profiles and stall notes.
        main = _jobs(files[".github/workflows/main.yml"])
        uploads = _upload_paths(main["linux-self-hosted"])
        self.assertEqual(1, len(uploads))
        self.assertEqual(SELF_HOSTED_UPLOAD_FILES,
                         {entry.rsplit("/", 1)[-1] for entry in uploads[0]})

    def test_upload_policy_rejects_raw_logs_directories_and_workspace_roots(self):
        files = self.supply_chain_files
        main = ".github/workflows/main.yml"
        coverage = ".github/workflows/coverage.yml"
        sanitizers = ".github/workflows/sanitizers.yml"
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
            "credential job": (sanitizers, files[sanitizers].replace(
                evidence, upload + evidence, 1), "after writing a cache credential"),
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
