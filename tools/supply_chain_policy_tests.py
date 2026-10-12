"""Keeps third-party fetches bound to reviewed bytes and third-party telemetry switched off.

Every archive fetch in Donner's own Starlark must carry a non-empty integrity hash. "Donner's own
Starlark" is MODULE.bazel plus every Donner module extension and repository rule file it declares,
followed through their loads; a file outside this test's data fails the test, so a new extension
cannot add a fetch that nothing checks. A git fetch has no integrity hash at all and a tag can be
moved, so new git fetches are rejected. The commit-pinned git fetches that predate this check are
listed in LEGACY_GIT_COMMIT_PINS until each moves to a release tag fetched as an archive with its
hash. That list may only shrink: each entry's digest must be in REVIEWED_LEGACY_PIN_DIGESTS, a
listed fetch may not change its remote or commit or share its name with a second fetch, and a
listed fetch that no longer exists fails until its entry and approval digest are deleted.

The check fails closed where it could otherwise be bypassed: a load or use_repo_rule of Bazel's
http.bzl or git.bzl must bind each rule under its own name, MODULE.bazel may not include() other
files, and main-repository labels (`@//`, `@@//`, `@donner//`) are followed like `//` labels.

The telemetry checks keep aspect_tools_telemetry's documented opt-out in the shared .bazelrc and in
every workflow's environment. The telemetry section of .bazelrc explains why.
"""

import ast
from hashlib import sha256
from pathlib import Path
import re
import unittest

from python.runfiles import runfiles


NATIVE_DOWNLOAD_APIS = frozenset({"download", "download_and_extract"})
HTTP_FETCH_RULES = frozenset({"archive_override", "http_archive", "http_file", "http_jar"}) | \
    NATIVE_DOWNLOAD_APIS
GIT_FETCH_RULES = frozenset({"git_override", "git_repository", "new_git_repository"})
HASH_PATTERNS = {
    "sha256": re.compile(r"[0-9a-f]{64}"),
    "integrity": re.compile(r"sha(?:256|384|512)-[A-Za-z0-9+/]+={0,2}"),
}
FULL_COMMIT = re.compile(r"[0-9a-f]{40}")

# Commit-pinned git fetches that predate this check: (declaring file, name) -> (remote, commit).
# Each is an open #1423 finding: Donner pins release tags or versions, so move each one to a release
# tag fetched as the tag archive or release asset with its integrity hash, then delete its entry
# here and its corresponding approval digest below. Never add or change an entry.
LEGACY_GIT_COMMIT_PINS = {
    ("MODULE.bazel", "bloaty"): (
        "https://github.com/jwmcglynn/bloaty", "a655c418811ef5097e37a562ce62f925bda72d88"),
    ("MODULE.bazel", "glfw"): (
        "https://github.com/glfw/glfw", "92dcf4ce74f2e2554a98fea09be7c705c17daa5a"),
    ("MODULE.bazel", "hedron_compile_commands"): (
        "https://github.com/hedronvision/bazel-compile-commands-extractor",
        "abb61a688167623088f8768cc9264798df6a9d10"),
    ("MODULE.bazel", "imgui"): (
        "https://github.com/ocornut/imgui", "ee1deccc08c14ab1e6abcfa6aae11d915763cdb7"),
    ("third_party/bazel/non_bcr_deps.bzl", "bazel_clang_tidy"): (
        "https://github.com/erenon/bazel_clang_tidy.git",
        "c4d35e0d0b838309358e57a2efed831780f85cd0"),
    ("third_party/bazel/non_bcr_deps.bzl", "woff2"): (
        "https://github.com/google/woff2.git", "fb9c3379f2605b10f3e8f1d9636664ab5576775c"),
}

# SHA-256 of "file|name|remote|commit" for each entry above as reviewed in #1423. An entry whose
# digest is missing here was added or changed after the review, which this test rejects. Keep
# this set paired with the active entries: delete its digest when retiring a pin.
REVIEWED_LEGACY_PIN_DIGESTS = frozenset({
    "00f2be3717e043f6027cbc15859850cb9a18778482660fab2f90521c6f87bba5",
    "50aec4cd196a494b72acc67da399119ef2ca77444c7799b6cff38acdbc83532f",
    "64ac4c7c678752a5c4a3a7a357ca3dc2f43abd7e0a6704c85da429bac3a37200",
    "936153b5d9f4423c45408fd82747e8c28d8e6801a73d1c7337f3bc7935a311fd",
    "f7101bf9d7ec27b36231d03d61064891b5c48054429a3681a7f9131dc437c6d5",
    "fa8f2ed5beed799036349282e640f7548b9fca804fc631e4adc511f12ee9f361",
})

# Bazel's own fetch rule files, whose rules must keep their canonical names wherever bound.
FETCH_RULE_FILES = re.compile(r"(?:@@?bazel_tools)?//tools/build_defs/repo:(?:http|git)\.bzl")
MAIN_REPOSITORY_PREFIXES = ("@@//", "@//", "@donner//")

TELEMETRY_OPT_OUT = {"DO_NOT_TRACK": "1", "ASPECT_TOOLS_TELEMETRY": "-all"}
TELEMETRY_VARIABLE = re.compile(r"DO_NOT_TRACK|ASPECT_TOOLS_TELEMETRY\w*")


def _runfile(path):
    resolved = Path(runfiles.Create().Rlocation("donner/%s" % path))
    if not resolved.is_file():
        raise AssertionError(
            "%s is not in //tools:supply_chain_policy_tests data; add it so its fetches are "
            "checked" % path
        )
    return resolved.read_text(encoding="utf-8")


def _literal(node):
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    return None


def _keyword(call, name):
    for keyword in call.keywords:
        if keyword.arg == name:
            return keyword.value
    return None


def _call_name(call):
    if isinstance(call.func, ast.Name):
        return call.func.id
    if isinstance(call.func, ast.Attribute) and call.func.attr in NATIVE_DOWNLOAD_APIS:
        return call.func.attr
    return None


def _calls(tree, functions):
    for node in ast.walk(tree):
        if isinstance(node, ast.Call) and _call_name(node) in functions:
            yield node


def _resolve_label(label, package):
    """Returns (path, package) for a Donner-owned label, or None for another repository's."""
    for prefix in MAIN_REPOSITORY_PREFIXES:
        if label.startswith(prefix):
            label = "//" + label[len(prefix):]
    if label.startswith(":"):
        target_package, name = package, label[1:]
    elif label.startswith("//"):
        target_package, _, name = label[2:].partition(":")
    else:
        return None
    path = "%s/%s" % (target_package, name) if target_package else name
    return path, target_package


def _referenced_files(tree, package):
    """Donner files a Starlark file loads, or declares as a module extension or repository rule."""
    for call in _calls(tree, {"load", "use_extension", "use_repo_rule"}):
        label = _literal(call.args[0]) if call.args else None
        resolved = _resolve_label(label, package) if label else None
        if resolved:
            yield resolved


def starlark_closure(read):
    """Parses MODULE.bazel and every Donner Starlark file it reaches."""
    pending = [("MODULE.bazel", "")]
    trees = {}
    while pending:
        path, package = pending.pop()
        if path not in trees:
            trees[path] = ast.parse(read(path), filename=path)
            pending.extend(_referenced_files(trees[path], package))
    return trees


def _loop_bindings(tree):
    """Maps every node inside `for item in NAME:` to (item, NAME)."""
    loops = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.For) and isinstance(node.target, ast.Name) and \
                isinstance(node.iter, ast.Name):
            loops.update((id(child), (node.target.id, node.iter.id)) for child in ast.walk(node))
    return loops


def _list_assignments(tree):
    """Maps each name to the element lists of every literal list assigned to it."""
    lists = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.Assign) and isinstance(node.value, (ast.List, ast.Tuple)):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    lists.setdefault(target.id, []).append(node.value.elts)
    return lists


class _Bindings:
    """Resolves `item.field` inside `for item in NAME:` to NAME's literal struct() fields."""

    def __init__(self, tree):
        self._loops = _loop_bindings(tree)
        self._lists = _list_assignments(tree)

    def values(self, expression, call):
        """Every string the expression takes, or None when it is not a readable literal."""
        literal = _literal(expression)
        if literal is not None:
            return [literal]
        if not (isinstance(expression, ast.Attribute) and isinstance(expression.value, ast.Name)):
            return None
        item, name = self._loops.get(id(call), (None, None))
        assignments = self._lists.get(name, [])
        if item != expression.value.id or len(assignments) != 1:
            return None
        return self._struct_fields(assignments[0], expression.attr)

    @staticmethod
    def _struct_fields(elements, field):
        values = []
        for element in elements:
            if not (isinstance(element, ast.Call) and isinstance(element.func, ast.Name) and
                    element.func.id == "struct"):
                return None
            value = _literal(_keyword(element, field))
            if value is None:
                return None
            values.append(value)
        return values


def _hash_problem(name, values):
    if values is None:
        return "%s is not a literal this check can read" % name
    if not values or not all(HASH_PATTERNS[name].fullmatch(value) for value in values):
        return "%s is empty or malformed" % name
    return None


def _http_problems(call, bindings):
    hashes = {name: _keyword(call, name) for name in HASH_PATTERNS}
    native = _call_name(call) in NATIVE_DOWNLOAD_APIS
    if native and hashes["sha256"] is None and len(call.args) > 2:
        hashes["sha256"] = call.args[2]
    # Both native APIs put sha256 third; an empty default can accompany a supplied integrity.
    if native and _literal(hashes["sha256"]) == "" and hashes["integrity"] is not None:
        hashes["sha256"] = None
    present = {name: expression for name, expression in hashes.items() if expression is not None}
    if not present:
        return ["has no sha256 or integrity"]
    problems = (_hash_problem(name, bindings.values(expression, call))
                for name, expression in present.items())
    return [problem for problem in problems if problem]


def _git_name(call):
    return _literal(_keyword(call, "module_name")) or _literal(_keyword(call, "name"))


def legacy_pin_digest(key, value):
    return sha256("|".join(key + value).encode("utf-8")).hexdigest()


def legacy_list_problems(pins, reviewed_digests=None):
    """Keep active pins and approvals paired, so retired pins cannot be restored unchanged."""
    reviewed = REVIEWED_LEGACY_PIN_DIGESTS if reviewed_digests is None else reviewed_digests
    active = {legacy_pin_digest(key, value) for key, value in pins.items()}
    problems = ["%s:%s is not a reviewed legacy pin" % key
                for key, value in sorted(pins.items()) if legacy_pin_digest(key, value) not in reviewed]
    if problems:
        return problems
    return ["retired legacy approval has no active entry; delete its digest: %s" % digest
            for digest in sorted(reviewed - active)]


def missing_legacy_fetches(pins, fetched):
    """Listed fetches that no longer exist; deleting their entries is how the list shrinks."""
    return ["%s:%s is gone; delete it from LEGACY_GIT_COMMIT_PINS" % key
            for key in sorted(set(pins) - set(fetched))]


def _git_problems(call, path, pins):
    name = _git_name(call)
    expected = pins.get((path, name))
    if expected is None:
        return [
            "%s carries no integrity hash; fetch the release tag's archive or release asset "
            "with its sha256 instead" % (name or "this fetch")
        ]
    actual = (_literal(_keyword(call, "remote")), _literal(_keyword(call, "commit")))
    if actual != expected or _keyword(call, "tag") or _keyword(call, "branch"):
        return [
            "%s must keep its listed remote and commit, with no tag or branch; move it to a "
            "release tag archive with its sha256 instead" % name
        ]
    return []


def _fetch_rule_bindings(call):
    """(bound name, rule name) pairs a load or use_repo_rule of http.bzl or git.bzl creates."""
    if call.func.id == "load":
        return [(_literal(arg), _literal(arg)) for arg in call.args[1:]] + \
            [(keyword.arg, _literal(keyword.value)) for keyword in call.keywords]
    return []


def binding_problems(path, tree):
    """Fetch rules bound under another name, or included files, would escape the checks above."""
    problems = ["%s:%d: include() hides declarations from this check" % (path, call.lineno)
                for call in _calls(tree, {"include"})]
    for call in _calls(tree, {"load", "use_extension", "use_repo_rule"}):
        problems.extend(_label_problems(path, call))
    for node in ast.walk(tree):
        if isinstance(node, ast.Assign) and isinstance(node.value, ast.Call) and \
                isinstance(node.value.func, ast.Name) and node.value.func.id == "use_repo_rule":
            problems.extend(_repo_rule_problems(path, node))
    direct_calls = {id(node.func) for node in ast.walk(tree) if isinstance(node, ast.Call)}
    problems.extend("%s:%d: native %s must be called directly" % (path, node.lineno, node.attr)
                    for node in ast.walk(tree) if isinstance(node, ast.Attribute)
                    and node.attr in NATIVE_DOWNLOAD_APIS and id(node) not in direct_calls)
    return problems


def _label_problems(path, call):
    label = _literal(call.args[0]) if call.args else None
    if label is None:
        return ["%s:%d: %s label is not a literal" % (path, call.lineno, call.func.id)]
    if not FETCH_RULE_FILES.fullmatch(label):
        return []
    return ["%s:%d: %s binds %s as %s" % (path, call.lineno, label, rule, bound)
            for bound, rule in _fetch_rule_bindings(call) if bound != rule]


def _repo_rule_problems(path, node):
    args = [_literal(arg) for arg in node.value.args]
    if len(args) != 2 or None in args or not FETCH_RULE_FILES.fullmatch(args[0]):
        return []
    targets = [target.id for target in node.targets if isinstance(target, ast.Name)]
    if targets == [args[1]] and len(node.targets) == 1:
        return []
    return ["%s:%d: use_repo_rule of %s must be bound as %s"
            % (path, node.lineno, args[1], args[1])]


def fetch_findings(trees, pins=None):
    """Returns (problems, git fetches present by (file, name)) for the parsed Starlark files."""
    pins = LEGACY_GIT_COMMIT_PINS if pins is None else pins
    problems = legacy_list_problems(pins)
    git_fetches = []
    for path, tree in sorted(trees.items()):
        bindings = _Bindings(tree)
        for call in _calls(tree, HTTP_FETCH_RULES | GIT_FETCH_RULES):
            if _call_name(call) in HTTP_FETCH_RULES:
                found = _http_problems(call, bindings)
            else:
                found = _git_problems(call, path, pins)
                git_fetches.append((path, _git_name(call)))
            problems.extend("%s:%d: %s %s" % (path, call.lineno, _call_name(call), problem)
                            for problem in found)
        problems.extend(_integrity_map_problems(path, tree))
        problems.extend(binding_problems(path, tree))
    problems.extend("%s:%s is fetched more than once" % key for key in sorted(set(git_fetches))
                    if git_fetches.count(key) > 1)
    return problems, set(git_fetches)


def _integrity_map_problems(path, tree):
    """An empty value in a browser integrity map would silently skip that archive's check."""
    problems = []
    for node in ast.walk(tree):
        if not (isinstance(node, ast.keyword) and node.arg == "integrity_path_map"):
            continue
        if not isinstance(node.value, ast.Dict):
            problems.append("%s:%d: integrity_path_map is not a literal" % (path, node.lineno))
            continue
        for key, value in zip(node.value.keys, node.value.values):
            literal = _literal(value)
            if literal is None or not HASH_PATTERNS["integrity"].fullmatch(literal):
                problems.append("%s:%d: integrity for %s is empty or malformed"
                                % (path, value.lineno, _literal(key)))
    return problems


def bazelrc_telemetry_problems(text):
    """The opt-out is unconditional, and nothing else in the file sets the same variables."""
    expected = {"common --repo_env=%s=%s" % item for item in TELEMETRY_OPT_OUT.items()}
    lines = [line.strip() for line in text.splitlines() if not line.lstrip().startswith("#")]
    mentions = [line for line in lines if TELEMETRY_VARIABLE.search(line)]
    problems = ["missing `%s`" % line for line in sorted(expected - set(mentions))]
    problems.extend("unexpected telemetry setting `%s`" % line
                    for line in mentions if line not in expected)
    return problems


def _top_level_env(lines):
    if "env:" not in lines:
        return []
    block = []
    for line in lines[lines.index("env:") + 1:]:
        if line and not line[0].isspace():
            break
        block.append(line)
    return block


def workflow_telemetry_problems(text):
    """The workflow-level env opts out, and no job, step or command overrides it."""
    lines = text.splitlines()
    env = _top_level_env(lines)
    problems = ["top-level env does not set %s: \"%s\"" % item for item in TELEMETRY_OPT_OUT.items()
                if '  %s: "%s"' % item not in env]
    expected = {'  %s: "%s"' % item for item in TELEMETRY_OPT_OUT.items()}
    problems.extend("unexpected telemetry setting `%s`" % line.strip() for line in lines
                    if TELEMETRY_VARIABLE.search(line) and line not in expected)
    return problems


def _workflows():
    main = Path(runfiles.Create().Rlocation("donner/.github/workflows/main.yml"))
    return {path.name: path.read_text(encoding="utf-8")
            for path in sorted(main.parent.glob("*.y*ml"))}


class CheckerTest(unittest.TestCase):
    """The checks reject the defects they exist for, so a green run is not vacuous."""

    @staticmethod
    def _findings(source, path="third_party/example.bzl"):
        return fetch_findings({path: ast.parse(source)})[0]

    def test_rejects_an_archive_without_a_hash(self):
        self.assertEqual(len(self._findings('http_archive(name = "a", urls = ["https://x"])')), 1)

    def test_rejects_empty_and_malformed_hashes(self):
        self.assertEqual(len(self._findings('http_archive(name = "a", sha256 = "")')), 1)
        self.assertEqual(len(self._findings('http_file(name = "a", integrity = "sha256-")')), 1)

    def test_reads_hashes_from_literal_structs_in_a_loop(self):
        source = 'P = [struct(sha256 = "%s"), struct(sha256 = "%s")]\nfor p in P:\n' \
                 '    http_archive(name = "a", sha256 = p.sha256)\n'
        self.assertEqual(self._findings(source % ("a" * 64, "b" * 64)), [])
        self.assertEqual(len(self._findings(source % ("a" * 64, ""))), 1)

    def test_rejects_a_hash_it_cannot_read(self):
        self.assertEqual(len(self._findings('http_archive(name = "a", sha256 = HASH)')), 1)

    def test_rejects_new_git_fetches_even_when_pinned(self):
        self.assertEqual(len(self._findings(
            'git_repository(name = "a", remote = "https://x", commit = "%s")' % ("a" * 40))), 1)
        self.assertEqual(len(self._findings(
            'new_git_repository(name = "a", remote = "https://x", tag = "1.0")')), 1)

    _WOFF2 = ("https://github.com/google/woff2.git", "fb9c3379f2605b10f3e8f1d9636664ab5576775c")
    _NON_BCR = "third_party/bazel/non_bcr_deps.bzl"

    def _woff2(self, **changes):
        remote = changes.get("remote", self._WOFF2[0])
        commit = changes.get("commit", self._WOFF2[1])
        extra = changes.get("extra", "")
        return 'new_git_repository(name = "woff2", remote = "%s", commit = "%s"%s)\n' % (
            remote, commit, extra)

    def test_legacy_git_fetch_must_keep_its_remote_and_commit(self):
        self.assertEqual(self._findings(self._woff2(), self._NON_BCR), [])
        for changed in (self._woff2(remote="https://example.com/woff2.git"),
                        self._woff2(commit="a" * 40),
                        self._woff2(extra=', tag = "v1.0.2"')):
            self.assertEqual(len(self._findings(changed, self._NON_BCR)), 1)

    def test_removed_legacy_fetch_must_leave_the_list(self):
        pins = {("x.bzl", "woff2"): self._WOFF2}
        self.assertEqual(missing_legacy_fetches(pins, {("x.bzl", "woff2")}), [])
        self.assertEqual(len(missing_legacy_fetches(pins, set())), 1)

    def test_legacy_name_may_not_be_fetched_twice(self):
        self.assertEqual(len(self._findings(self._woff2() * 2, self._NON_BCR)), 1)

    def test_legacy_list_rejects_added_or_changed_entries(self):
        self.assertEqual(legacy_list_problems(LEGACY_GIT_COMMIT_PINS), [])
        added = dict(LEGACY_GIT_COMMIT_PINS)
        added[("MODULE.bazel", "new_dep")] = ("https://example.com/x", "a" * 40)
        self.assertEqual(len(legacy_list_problems(added)), 1)
        changed = dict(LEGACY_GIT_COMMIT_PINS)
        changed[("third_party/bazel/non_bcr_deps.bzl", "woff2")] = (self._WOFF2[0], "a" * 40)
        self.assertEqual(len(legacy_list_problems(changed)), 1)
        shrunk = dict(LEGACY_GIT_COMMIT_PINS)
        del shrunk[("MODULE.bazel", "imgui")]
        retired_digest = legacy_pin_digest(("MODULE.bazel", "imgui"),
                                           LEGACY_GIT_COMMIT_PINS[("MODULE.bazel", "imgui")])
        self.assertEqual(legacy_list_problems(
            shrunk, REVIEWED_LEGACY_PIN_DIGESTS - {retired_digest}), [])

    def test_fetch_rules_must_keep_their_canonical_names(self):
        canonical = ('http_file = use_repo_rule("@bazel_tools//tools/build_defs/repo:http.bzl", '
                     '"http_file")\n'
                     'load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")\n')
        self.assertEqual(self._findings(canonical), [])
        for renamed in (
                'fetch = use_repo_rule("@bazel_tools//tools/build_defs/repo:http.bzl", '
                '"http_file")',
                'load("@bazel_tools//tools/build_defs/repo:http.bzl", fetch = "http_archive")',
                'load("@@bazel_tools//tools/build_defs/repo:git.bzl", g = "git_repository")'):
            self.assertEqual(len(self._findings(renamed)), 1, renamed)

    def test_rejects_include_and_non_literal_labels(self):
        self.assertEqual(len(self._findings('include("//:more.MODULE.bazel")', "MODULE.bazel")), 1)
        self.assertEqual(len(self._findings('ext = use_extension(LABEL, "x")')), 1)

    def test_main_repository_labels_are_followed(self):
        for label in ("@@//third_party:x.bzl", "@donner//third_party:x.bzl",
                      "@//third_party:x.bzl", "//third_party:x.bzl"):
            self.assertEqual(_resolve_label(label, ""), ("third_party/x.bzl", "third_party"))
        self.assertIsNone(_resolve_label("@rules_cc//cc:defs.bzl", ""))
        files = {"MODULE.bazel": 'ext = use_extension("@donner//third_party:x.bzl", "x")',
                 "third_party/x.bzl": 'http_archive(name = "a")'}
        trees = starlark_closure(files.__getitem__)
        self.assertEqual(set(trees), set(files))
        self.assertEqual(len(fetch_findings(trees)[0]), 1)

    def test_rejects_an_empty_browser_integrity(self):
        source = 'playwright.repo(integrity_path_map = {"a.zip": "sha256-AAAA", "b.zip": ""})'
        self.assertEqual(len(self._findings(source)), 1)

    def test_bazelrc_rejects_a_missing_empty_or_scoped_setting(self):
        complete = ("common --repo_env=DO_NOT_TRACK=1\n"
                    "common --repo_env=ASPECT_TOOLS_TELEMETRY=-all\n")
        self.assertEqual(bazelrc_telemetry_problems(complete), [])
        self.assertEqual(len(bazelrc_telemetry_problems("common --repo_env=DO_NOT_TRACK=1\n")), 1)
        reenabled = complete + "build:ci --repo_env=ASPECT_TOOLS_TELEMETRY=\n"
        self.assertEqual(len(bazelrc_telemetry_problems(reenabled)), 1)

    def test_workflow_rejects_missing_env_and_overrides(self):
        complete = ('name: X\nenv:\n  DO_NOT_TRACK: "1"\n'
                    '  ASPECT_TOOLS_TELEMETRY: "-all"\non: push\n')
        self.assertEqual(workflow_telemetry_problems(complete), [])
        self.assertEqual(len(workflow_telemetry_problems("name: X\non: push\n")), 2)
        override = complete + "jobs:\n  a:\n    env:\n      DO_NOT_TRACK: \"\"\n"
        self.assertEqual(len(workflow_telemetry_problems(override)), 1)


    def test_native_repository_downloads_require_an_integrity_hash(self):
        for context in ("ctx", "repository_ctx", "download_context"):
            for method in ("download", "download_and_extract"):
                for suffix in ("", ', sha256 = ""', ', integrity = "sha256-"',
                               ", sha256 = UNKNOWN_HASH"):
                    with self.subTest(context=context, method=method, suffix=suffix):
                        source = '%s.%s("https://example.invalid/archive"%s)' % (
                            context, method, suffix)
                        self.assertEqual(len(self._findings(source)), 1)
                valid = '%s.%s("https://example.invalid/archive", sha256 = "%s")' % (
                    context, method, "a" * 64)
                self.assertEqual(self._findings(valid), [])
                positional = '%s.%s("https://example.invalid/archive", "out", "%s")' % (
                    context, method, "a" * 64)
                self.assertEqual(self._findings(positional), [])

    def test_retired_legacy_pin_cannot_keep_a_dormant_approval(self):
        retired = dict(LEGACY_GIT_COMMIT_PINS)
        del retired[(self._NON_BCR, "woff2")]
        self.assertEqual(len(legacy_list_problems(retired)), 1)


    def test_retirement_rejects_restoring_the_original_pin(self):
        key = (self._NON_BCR, "woff2")
        retired = dict(LEGACY_GIT_COMMIT_PINS)
        value = retired.pop(key)
        remaining_approvals = REVIEWED_LEGACY_PIN_DIGESTS - {legacy_pin_digest(key, value)}
        self.assertEqual(legacy_list_problems(retired, remaining_approvals), [])
        retired[key] = value
        self.assertEqual(len(legacy_list_problems(retired, remaining_approvals)), 1)

    def test_native_integrity_supports_the_empty_positional_sha_default(self):
        integrity = "sha256-" + "A" * 43 + "="
        for method in NATIVE_DOWNLOAD_APIS:
            source = 'ctx.%s("https://example.invalid/archive", "out", "", ' \
                     'integrity = "%s")' % (method, integrity)
            self.assertEqual(self._findings(source), [])

    def test_native_download_aliases_cannot_hide_a_fetch(self):
        for method in NATIVE_DOWNLOAD_APIS:
            source = 'fetch = ctx.%s\nfetch("https://example.invalid/archive")' % method
            self.assertEqual(len(self._findings(source)), 1)


class RepositoryPolicyTest(unittest.TestCase):
    """Applies the checks to the checked-in configuration."""

    @classmethod
    def setUpClass(cls):
        cls.trees = starlark_closure(_runfile)
        cls.problems, cls.legacy = fetch_findings(cls.trees)

    def test_closure_reaches_the_dependency_extensions(self):
        self.assertTrue({"MODULE.bazel", "third_party/bazel/non_bcr_deps.bzl",
                         "third_party/google_fonts/fonts.bzl"} <= set(self.trees))

    def test_every_archive_fetch_carries_an_integrity_hash(self):
        self.assertEqual(self.problems, [])

    def test_legacy_git_commit_pins_only_shrink(self):
        self.assertEqual(missing_legacy_fetches(LEGACY_GIT_COMMIT_PINS, self.legacy), [])

    def test_bazelrc_opts_out_of_telemetry(self):
        self.assertEqual(bazelrc_telemetry_problems(_runfile(".bazelrc")), [])

    def test_every_workflow_opts_out_of_telemetry(self):
        workflows = _workflows()
        self.assertGreater(len(workflows), 10)
        problems = {name: found for name, text in workflows.items()
                    if (found := workflow_telemetry_problems(text))}
        self.assertEqual(problems, {})


if __name__ == "__main__":
    unittest.main()
