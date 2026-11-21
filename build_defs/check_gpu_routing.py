#!/usr/bin/env python3
"""Keeps remote GPU requests in reviewed target platforms and the Linux GPU config off by default.

A remote execution request for a platform property that no executor in the pool provides is not
an error: the action is queued until such an executor appears, and the job waits until its own
timeout with no failing action to point at. A Linux test may therefore ask for a GPU only through
`--config=ci-linux-gpu` on a command line that also names the remote instance with the GPU
executor. This check enforces, in tracked files:

Starlark (BUILD files, .bzl files, MODULE.bazel and WORKSPACE files), read with Python's parser:
  - a `gpu` execution property (a dict key `gpu` or `<exec group>.gpu`, written literally, through
    a constant assigned in the same file, or as `dict(gpu = ...)`) appears only in the
    `exec_properties` of the reviewed platform definitions in tools/ci/BUILD.bazel;
  - no platform inherits a reviewed GPU platform;
  - the Linux GPU platform's name appears only in its own definition, so no transition, config
    setting or registration can select it;
  - `register_execution_platforms` names no reviewed GPU platform and no target pattern.

rc files (every tracked `.bazelrc` and `*.bazelrc`):
  - no line includes `--config=ci-linux-gpu`: the config is for command lines only;
  - the Linux GPU platform is named only by the config's one `--platforms` line in .bazelrc;
  - no default execution property mentions `gpu`, in any spelling of the flag;
  - no execution or host platform flag names a reviewed GPU platform or a target pattern such as
    `:all`, `:*` or `...`, which could reach one.

Workflows, composite actions and tracked shell scripts:
  - a Bazel command that uses `--config=ci-linux-gpu` is a `build` or names
    `--remote_instance_name` on the same command line (continuation lines are joined; `&&`, `||`,
    `;` and `|` separate commands); a command whose Bazel verb cannot be read is rejected;
  - the default execution property and platform flag rules above, and no Linux GPU platform name.

Python lane code (tracked .py files under tools/ci and .github): no string other than a docstring
names the config or the Linux GPU platform, because a command assembled in Python cannot be
checked for the instance name.

Not checked, because no static reading can see it: strings built by concatenation or formatting,
keys or names loaded from another file, commands assembled in shell variables, and rc files that
are not tracked (a user's or a runner's rc file).

Usage: python3 build_defs/check_gpu_routing.py [FILE...]
With no arguments it checks every tracked file of those kinds; run it from the repository root.
"""

import ast
import re
import shlex
import subprocess
import sys

PLATFORMS_BUILD = "tools/ci/BUILD.bazel"
LINUX_GPU_PLATFORM = "linux_ci_gpu"
GPU_CONFIG = "ci-linux-gpu"
# Platforms that may ask for a GPU, by name in tools/ci/BUILD.bazel. Each is selected only as a
# target platform, so only test actions carry the request.
REVIEWED_GPU_PLATFORMS = ("remote_macos_aarch64_gpu", LINUX_GPU_PLATFORM)
# The only rc line that may name the Linux GPU platform.
LINUX_GPU_CONFIG_LINE = "build:%s --platforms=//tools/ci:%s" % (GPU_CONFIG, LINUX_GPU_PLATFORM)
STARLARK_NAMES = ("BUILD", "BUILD.bazel", "MODULE.bazel", "WORKSPACE", "WORKSPACE.bazel")

_CONFIG_USE = re.compile(r"""--config(?:=|\s+)["']?%s\b""" % re.escape(GPU_CONFIG))
_DEFAULT_PROPERTIES = re.compile(r"remote_default_(?:exec|platform)_properties")
_PLATFORM_FLAG = re.compile(
    r"--(?:extra_execution_platforms|host_platform)(?:=|\s+)[\"']?([^\s\"']+)")
_PATTERN = re.compile(r":all\b|:\*|\.\.\.")
_BAZEL_VERB = re.compile(r"\b(?:bazelisk|bazel)\s+(?:--\S+\s+)*([a-z_-]+)")
_SEPARATOR = re.compile(r"&&|\|\||;|\|")


def is_starlark(path):
    name = path.rsplit("/", 1)[-1]
    return name in STARLARK_NAMES or name.endswith((".bzl", ".MODULE.bazel"))


def is_rc(path):
    return path.rsplit("/", 1)[-1].endswith(".bazelrc")


def is_lane_python(path):
    return path.endswith(".py") and path.startswith(("tools/ci/", ".github/"))


def is_command_text(path):
    name = path.rsplit("/", 1)[-1]
    return ((path.startswith(".github/workflows/") and name.endswith((".yml", ".yaml")))
            or (path.startswith(".github/actions/") and name in ("action.yml", "action.yaml"))
            or (name.endswith(".sh") and not path.startswith("third_party/")))


def checked_path(path):
    """Whether the full check reads this tracked path."""
    return is_starlark(path) or is_rc(path) or is_lane_python(path) or is_command_text(path)


def _is_gpu_key(value):
    return isinstance(value, str) and (value == "gpu" or value.endswith(".gpu"))


def _keyword(call, name):
    return next((keyword.value for keyword in call.keywords if keyword.arg == name), None)


def _call_name(call):
    return getattr(call.func, "id", None) or getattr(call.func, "attr", None)


def _strings(node):
    """Every string constant in an expression."""
    return [child.value for child in ast.walk(node)
            if isinstance(child, ast.Constant) and isinstance(child.value, str)]


class _StarlarkChecker(ast.NodeVisitor):
    def __init__(self, path):
        self.path = path
        self.problems = []
        self.constants = {}
        self.exempt = set()

    def report(self, node, message):
        self.problems.append("%s:%d: %s" % (self.path, node.lineno, message))

    def visit_Module(self, node):
        for statement in node.body:
            if (isinstance(statement, ast.Assign) and isinstance(statement.value, ast.Constant)
                    and isinstance(statement.value.value, str)):
                for target in statement.targets:
                    if isinstance(target, ast.Name):
                        self.constants[target.id] = statement.value.value
        self.generic_visit(node)

    def _key_value(self, key):
        if isinstance(key, ast.Constant):
            return key.value
        if isinstance(key, ast.Name):
            return self.constants.get(key.id)
        return None

    def _reviewed_platform(self, call):
        name = _keyword(call, "name")
        return (self.path == PLATFORMS_BUILD and _call_name(call) == "platform"
                and isinstance(name, ast.Constant) and name.value in REVIEWED_GPU_PLATFORMS)

    def visit_Call(self, node):
        name = _call_name(node)
        if self._reviewed_platform(node):
            self._exempt_reviewed_platform(node)
        if name == "platform":
            self._check_parents(node)
        elif name == "register_execution_platforms":
            self._check_registration(node)
        elif name == "dict":
            self._check_dict_call(node)
        self.generic_visit(node)

    def _exempt_reviewed_platform(self, node):
        for attribute in ("exec_properties", "name"):
            value = _keyword(node, attribute)
            if value is not None:
                self.exempt.add(id(value))

    def _check_parents(self, node):
        parents = _keyword(node, "parents")
        values = _strings(parents) if parents is not None else []
        if any(value.rsplit(":", 1)[-1] in REVIEWED_GPU_PLATFORMS for value in values):
            self.report(node, "a platform inherits a reviewed GPU platform")

    def _check_registration(self, node):
        for value in _strings(node):
            if _PATTERN.search(value) or value.rsplit(":", 1)[-1] in REVIEWED_GPU_PLATFORMS:
                self.report(node, "registers a GPU platform or a target pattern as an execution "
                                  "platform")

    def _check_dict_call(self, node):
        if any(keyword.arg == "gpu" for keyword in node.keywords) and not self._within_exempt(node):
            self.report(node, "a `gpu` execution property outside the reviewed GPU platforms of %s"
                        % PLATFORMS_BUILD)

    def _within_exempt(self, node):
        return id(node) in self.exempt

    def visit_Dict(self, node):
        if not self._within_exempt(node):
            for key in node.keys:
                if key is not None and _is_gpu_key(self._key_value(key)):
                    self.report(key, "a `gpu` execution property outside the reviewed GPU "
                                     "platforms of %s; select a GPU target platform instead"
                                % PLATFORMS_BUILD)
        self.generic_visit(node)

    def visit_Constant(self, node):
        if (isinstance(node.value, str) and LINUX_GPU_PLATFORM in node.value
                and not self._within_exempt(node)):
            self.report(node, "the Linux GPU platform is named outside its definition; only "
                              "`--config=%s` may select it" % GPU_CONFIG)


def _starlark_problems(path, text):
    try:
        tree = ast.parse(text)
    except SyntaxError as error:
        return ["%s:%s: cannot be parsed, so its GPU routing cannot be checked"
                % (path, error.lineno)]
    checker = _StarlarkChecker(path)
    checker.visit(tree)
    return checker.problems


def _uncommented(line):
    return line.split(" #", 1)[0] if not line.lstrip().startswith("#") else ""


def _text_rules(path, number, line):
    """Default property and platform flag problems on one uncommented line."""
    problems = []
    if _DEFAULT_PROPERTIES.search(line) and "gpu" in line:
        problems.append("%s:%d: a default `gpu` execution property reaches every action"
                        % (path, number))
    for value in _PLATFORM_FLAG.findall(line):
        if _PATTERN.search(value) or any(name in value for name in REVIEWED_GPU_PLATFORMS):
            problems.append("%s:%d: an execution or host platform flag names a GPU platform or "
                            "a target pattern" % (path, number))
    return problems


def _rc_problems(path, text):
    problems = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = _uncommented(raw).strip()
        if not line:
            continue
        if _CONFIG_USE.search(line):
            problems.append("%s:%d: an rc line includes `--config=%s`; the config belongs only on "
                            "a command line that names the GPU instance" % (path, number, GPU_CONFIG))
        if LINUX_GPU_PLATFORM in line and not (path == ".bazelrc" and line == LINUX_GPU_CONFIG_LINE):
            problems.append("%s:%d: the Linux GPU platform may only be the target platform that "
                            "`--config=%s` selects" % (path, number, GPU_CONFIG))
        problems += _text_rules(path, number, line)
    return problems


def _has_instance(command):
    """Whether the final instance option supplies a nonempty argument after shell quoting."""
    try:
        words = shlex.split(command)
    except ValueError:
        return False
    value = ""
    for index, word in enumerate(words):
        if word == "--":
            break
        if word.startswith("--remote_instance_name="):
            value = word.partition("=")[2]
        elif word == "--remote_instance_name":
            value = words[index + 1] if index + 1 < len(words) else ""
    return bool(value) and not value.startswith("--")


def _command_problems(path, number, command):
    if not _CONFIG_USE.search(command):
        return []
    verb = _BAZEL_VERB.search(command)
    if verb is None:
        return ["%s:%d: `--config=%s` in a command whose Bazel verb cannot be read"
                % (path, number, GPU_CONFIG)]
    if verb.group(1) == "build" or _has_instance(command):
        return []
    return ["%s:%d: `bazel %s --config=%s` without `--remote_instance_name` would queue its tests "
            "where no Linux GPU executor runs" % (path, number, verb.group(1), GPU_CONFIG)]


def _command_text_problems(path, text):
    problems = []
    joined = re.sub(r"\\\n", " ", text)
    lines = joined.splitlines()
    for number, raw in enumerate(lines, 1):
        line = _uncommented(raw)
        if LINUX_GPU_PLATFORM in line:
            problems.append("%s:%d: the Linux GPU platform is named outside its definition"
                            % (path, number))
        problems += _text_rules(path, number, line)
        for command in _SEPARATOR.split(line):
            problems += _command_problems(path, number, command)
    return problems


def _docstrings(tree):
    nodes = [tree] + [node for node in ast.walk(tree)
                      if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef))]
    found = set()
    for node in nodes:
        body = getattr(node, "body", [])
        if (body and isinstance(body[0], ast.Expr) and isinstance(body[0].value, ast.Constant)
                and isinstance(body[0].value.value, str)):
            found.add(id(body[0].value))
    return found


def _python_problems(path, text):
    try:
        tree = ast.parse(text)
    except SyntaxError as error:
        return ["%s:%s: cannot be parsed, so its GPU routing cannot be checked"
                % (path, error.lineno)]
    docstrings = _docstrings(tree)
    problems = []
    for node in ast.walk(tree):
        if (isinstance(node, ast.Constant) and isinstance(node.value, str)
                and id(node) not in docstrings
                and (GPU_CONFIG in node.value or LINUX_GPU_PLATFORM in node.value)):
            problems.append("%s:%d: lane code names the Linux GPU config or platform; a command "
                            "assembled in Python cannot be checked for the instance name"
                            % (path, node.lineno))
    return problems


def problems(files):
    """Problems of file texts, keyed by repository path."""
    found = []
    for path, text in sorted(files.items()):
        if is_starlark(path):
            found += _starlark_problems(path, text)
        elif is_rc(path):
            found += _rc_problems(path, text)
        elif is_lane_python(path):
            found += _python_problems(path, text)
        elif is_command_text(path):
            found += _command_text_problems(path, text)
    return found


def _tracked_paths():
    listing = subprocess.run(["git", "ls-files", "-z"], check=True, capture_output=True).stdout
    return [path for path in listing.decode("utf-8").split("\0") if path and checked_path(path)]


def main(argv):
    files = {}
    for path in argv[1:] or _tracked_paths():
        with open(path, encoding="utf-8") as handle:
            files[path] = handle.read()
    found = problems(files)
    for problem in found:
        print(problem)
    print("Checked %d files for GPU routing: %d problem(s)." % (len(files), len(found)))
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
