#!/usr/bin/env python3
"""Keep remote GPU properties on test target platforms.

Only the reviewed GPU platforms may declare a GPU property. A Linux hardware test can select its
platform through the native test registration. Setting it as a default target, host, or execution
platform affects unrelated actions and can leave them waiting for a worker indefinitely.

The check covers literal Starlark properties and same-file key constants, platform registrations,
tracked rc files and command lines in workflows and scripts. Dynamic strings, imported constants
and untracked runner configuration require separate review.
"""

import ast
import re
import subprocess
import sys

PLATFORMS_BUILD = "tools/ci/BUILD.bazel"
LINUX_GPU_PLATFORM = "linux_ci_gpu"
LINUX_TEST_REGISTRATION = "build_defs/ci_linux_gpu.bzl"
# Platforms that may ask for a GPU, by name in tools/ci/BUILD.bazel. Each is selected only as a
# target platform, so only test actions carry the request.
REVIEWED_GPU_PLATFORMS = ("remote_macos_aarch64_gpu", LINUX_GPU_PLATFORM)
STARLARK_NAMES = ("BUILD", "BUILD.bazel", "MODULE.bazel", "WORKSPACE", "WORKSPACE.bazel")

_DEFAULT_PROPERTIES = re.compile(r"remote_default_(?:exec|platform)_properties")
_PLATFORM_FLAG = re.compile(
    r"--(?:extra_execution_platforms|host_platform)(?:=|\s+)[\"']?([^\s\"']+)")
_PATTERN = re.compile(r":all\b|:\*|\.\.\.")


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
        if self.path == LINUX_TEST_REGISTRATION and getattr(node.func, "id", None) == "ci_platform_test":
            target = _keyword(node, "target_platform")
            if isinstance(target, ast.Constant) and target.value == "//tools/ci:" + LINUX_GPU_PLATFORM:
                self.exempt.add(id(target))
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
            self.report(node, "the Linux GPU platform is named outside its definition or "
                              "native test registration")


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


def _routing_text_problems(path, text):
    problems = []
    for number, raw in enumerate(text.replace("\\\n", " ").splitlines(), 1):
        line = _uncommented(raw)
        if LINUX_GPU_PLATFORM in line:
            problems.append("%s:%d: only native test registration may select the Linux GPU platform"
                            % (path, number))
        problems += _text_rules(path, number, line)
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
                and LINUX_GPU_PLATFORM in node.value):
            problems.append("%s:%d: only native test registration may select the Linux GPU platform"
                            % (path, node.lineno))
    return problems


def problems(files):
    """Problems of file texts, keyed by repository path."""
    found = []
    for path, text in sorted(files.items()):
        if is_starlark(path):
            found += _starlark_problems(path, text)
        elif is_lane_python(path):
            found += _python_problems(path, text)
        elif is_rc(path) or is_command_text(path):
            found += _routing_text_problems(path, text)
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
