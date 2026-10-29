"""Exercise resource identity, package validation, and downstream generator dependencies."""
import ast
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest

from packaged_resources import generate, merge, resource_record


def _top_level_calls(source, filename):
    tree = ast.parse(source, filename=filename)
    return [statement.value for statement in tree.body
            if isinstance(statement, ast.Expr) and isinstance(statement.value, ast.Call)]


def _call_keywords(call):
    result = {}
    for keyword in call.keywords:
        if keyword.arg is None or keyword.arg in result:
            raise ValueError("Ambiguous declaration keywords")
        result[keyword.arg] = keyword.value
    return result


def _literal_string(node, description):
    if not isinstance(node, ast.Constant) or not isinstance(node.value, str) or not node.value:
        raise ValueError(description + " must be a nonempty literal string")
    return node.value


def _external_load_repository(call):
    if not isinstance(call.func, ast.Name) or call.func.id != "load":
        return None
    label = _literal_string(call.args[0] if call.args else None, "load label")
    if not label.startswith("@"):
        return None
    repository, separator, path = label[1:].partition("//")
    if not separator or not repository or repository.startswith("@") or not path:
        raise ValueError("Expected an apparent repository load label")
    return repository


def _generator_package_repositories(build_source):
    calls = _top_level_calls(build_source, "build_defs/BUILD.bazel")
    generators = [call for call in calls if isinstance(call.func, ast.Name)
                  and call.func.id == "py_binary"
                  and any(keyword.arg == "name" and isinstance(keyword.value, ast.Constant)
                          and keyword.value.value == "packaged_resources_generator"
                          for keyword in call.keywords)]
    if len(generators) != 1:
        raise ValueError("Expected one production packaged_resources_generator py_binary")
    repositories = set()
    for call in calls:
        repository = _external_load_repository(call)
        if repository is not None:
            repositories.add(repository)
    if not repositories:
        raise ValueError("Production generator package has no external rule sets")
    return repositories


def _module_repository_dependencies(module_source):
    dependencies = {}
    for call in _top_level_calls(module_source, "MODULE.bazel"):
        if not isinstance(call.func, ast.Name) or call.func.id != "bazel_dep":
            continue
        keywords = _call_keywords(call)
        name = _literal_string(keywords.get("name"), "bazel_dep name")
        repository = _literal_string(keywords.get("repo_name", ast.Constant(name)), "repo_name")
        flag = keywords.get("dev_dependency", ast.Constant(False))
        if not isinstance(flag, ast.Constant) or type(flag.value) is not bool:
            raise ValueError(repository + ": dev_dependency must be a literal boolean")
        if repository in dependencies:
            raise ValueError("Duplicate bazel_dep repository: " + repository)
        dependencies[repository] = flag.value
    return dependencies


def _production_generator_dependency_errors(module_source, build_source):
    """Check direct external loads in the production generator's BUILD package."""
    required = _generator_package_repositories(build_source)
    dependencies = _module_repository_dependencies(module_source)
    return [("Missing production dependency: " + repository) if repository not in dependencies
            else ("Development-only production dependency: " + repository)
            for repository in sorted(required)
            if repository not in dependencies or dependencies[repository]]


def _mutate_module_dependency(module_source, repository, replacement):
    tree = ast.parse(module_source)
    matches = []
    for statement in tree.body:
        if not isinstance(statement, ast.Expr) or not isinstance(statement.value, ast.Call):
            continue
        call = statement.value
        if not isinstance(call.func, ast.Name) or call.func.id != "bazel_dep":
            continue
        keywords = _call_keywords(call)
        name = _literal_string(keywords.get("name"), "bazel_dep name")
        if _literal_string(keywords.get("repo_name", ast.Constant(name)), "repo_name") == repository:
            matches.append(statement)
    if len(matches) != 1:
        raise ValueError("Expected one dependency to mutate: " + repository)
    if replacement is None:
        tree.body.remove(matches[0])
    else:
        call = matches[0].value
        call.keywords = [keyword for keyword in call.keywords if keyword.arg != "dev_dependency"]
        call.keywords.append(ast.keyword(arg="dev_dependency", value=replacement))
    return ast.unparse(ast.fix_missing_locations(tree))


class PackagedResourcesTest(unittest.TestCase):
    def test_canonical_serialization_matches_compiled_manifest_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "fixture.svg"
            source.write_bytes(b"abc")
            generate(dict(group="sample", runfilesRoot="unused", resources=[
                dict(id="kFixture", source=str(source))]), root / "Fixture.h", root / "set")
            expected = Path(__file__).with_name("testdata") / "packaged-resource-manifest.json"
            self.assertEqual((root / "set/manifest.json").read_bytes(), expected.read_bytes())

    def test_exact_payload_metadata_and_reuse_across_code_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "icon.svg"
            payload = b'<svg xmlns="http://www.w3.org/2000/svg"/>'
            source.write_bytes(payload)
            request = dict(group="chrome", runfilesRoot="_main/pkg/set.assets",
                           resources=[dict(id="kIconSvg", source=str(source))])
            generate(request, root / "Icon.h", root / "first")
            generate(request, root / "IconAgain.h", root / "second")
            first = json.loads((root / "first/manifest.json").read_text())
            second = json.loads((root / "second/manifest.json").read_text())
            self.assertEqual(first, second)
            record = first["assets"][0]
            self.assertEqual(record["sha256"], hashlib.sha256(payload).hexdigest())
            self.assertEqual((root / "first" / record["path"]).read_bytes(), payload)
            header = (root / "Icon.h").read_text()
            self.assertNotIn(payload.decode(), header)
            self.assertIn("PackagedResource kIconSvg", header)
            merge([root / "first", root / "second"], root / "bundle")
            self.assertEqual(json.loads((root / "bundle/manifest.json").read_text()), first)
            self.assertEqual(len(list((root / "bundle/assets").iterdir())), 1)

    def test_reject_corruption_and_conflicting_logical_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "font.ttf"
            source.write_bytes(b"encoded-font")
            request = dict(group="ui-font", runfilesRoot="_main/pkg/set.assets",
                           resources=[dict(id="kFontTtf", source=str(source))])
            generate(request, root / "Font.h", root / "first")
            source.write_bytes(b"changed-font")
            generate(request, root / "Changed.h", root / "changed")
            with self.assertRaisesRegex(ValueError, "Conflicting"):
                merge([root / "first", root / "changed"], root / "conflict")
            record = json.loads((root / "first/manifest.json").read_text())["assets"][0]
            (root / "first" / record["path"]).write_bytes(b"corrupt-font")
            with self.assertRaisesRegex(ValueError, "identity"):
                merge([root / "first"], root / "corrupt")

    def test_rejects_paths_schema_symlinks_and_extra_inventory_before_reading(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "icon.svg"
            source.write_bytes(b"svg-bytes")
            request = dict(group="chrome", runfilesRoot="_main/pkg/set.assets",
                           resources=[dict(id="kIconSvg", source=str(source))])
            generate(request, root / "Icon.h", root / "valid")
            manifest_path = root / "valid/manifest.json"
            original = json.loads(manifest_path.read_text())
            for key, value, reason in [
                    ("path", "../outside.svg", "Noncanonical"),
                    ("encodedSize", True, "size type"),
                    ("encodedSize", 17 * 1024 * 1024, "size"),
                    ("sha256", "bad", "digest"),
                    ("mediaType", "text/javascript", "media type")]:
                with self.subTest(key=key, value=value):
                    altered = json.loads(json.dumps(original))
                    altered["assets"][0][key] = value
                    manifest_path.write_text(json.dumps(altered))
                    with self.assertRaisesRegex(ValueError, reason):
                        merge([root / "valid"], root / ("bad-" + key + str(value)))
            duplicate = json.loads(json.dumps(original))
            duplicate["assets"].append(duplicate["assets"][0])
            manifest_path.write_text(json.dumps(duplicate))
            with self.assertRaisesRegex(ValueError, "Duplicate"):
                merge([root / "valid"], root / "duplicate")
            manifest_path.write_text(json.dumps(original))
            (root / "valid/extra.txt").write_text("unexpected")
            with self.assertRaisesRegex(ValueError, "inventory mismatch"):
                merge([root / "valid"], root / "extra")
            (root / "valid/extra.txt").unlink()
            asset = root / "valid" / original["assets"][0]["path"]
            asset.unlink()
            asset.symlink_to(source)
            with self.assertRaisesRegex(ValueError, "symlink"):
                merge([root / "valid"], root / "symlink")

    def test_bazel_merge_accepts_bound_sandbox_input_links(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            relative = Path("bazel-out/test/bin/pkg/input.assets")
            actual = root / "producer" / relative
            staged = root / "sandbox" / relative
            source = root / "icon.svg"
            source.write_bytes(b"svg-bytes")
            generate(dict(group="chrome", runfilesRoot="unused", resources=[
                dict(id="kIconSvg", source=str(source))]), root / "Icon.h", actual)
            staged.mkdir(parents=True)
            (staged / "assets").mkdir()
            for path in actual.rglob("*"):
                if path.is_file():
                    (staged / path.relative_to(actual)).symlink_to(path)
            previous = Path.cwd()
            try:
                os.chdir(root / "sandbox")
                with self.assertRaises((OSError, ValueError)):
                    merge([relative], root / "strict")
                merge([relative], root / "bundle", bazel_inputs=True)
                self.assertEqual((root / "bundle/manifest.json").read_bytes(),
                                 (actual / "manifest.json").read_bytes())
                record = json.loads((actual / "manifest.json").read_text())["assets"][0]
                self.assertEqual((root / "bundle" / record["path"]).read_bytes(), b"svg-bytes")
                (staged / record["path"]).unlink()
                (staged / record["path"]).symlink_to(source)
                with self.assertRaisesRegex(ValueError, "declared Bazel tree"):
                    merge([relative], root / "escape", bazel_inputs=True)
            finally:
                os.chdir(previous)

    def test_bazel_merge_rejects_undeclared_roots_and_inventory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            relative = Path("bazel-out/test/bin/pkg/input.assets")
            actual = root / "producer" / relative
            staged = root / "sandbox" / relative
            source = root / "icon.svg"
            source.write_bytes(b"svg-bytes")
            generate(dict(group="chrome", runfilesRoot="unused", resources=[
                dict(id="kIconSvg", source=str(source))]), root / "Icon.h", actual)
            staged.mkdir(parents=True)
            (staged / "assets").mkdir()
            for path in actual.rglob("*"):
                if path.is_file():
                    (staged / path.relative_to(actual)).symlink_to(path)
            previous = Path.cwd()
            try:
                os.chdir(root / "sandbox")
                for bad in (actual, Path("../producer") / relative):
                    with self.subTest(path=str(bad)), self.assertRaisesRegex(ValueError, "declared Bazel output"):
                        merge([bad], root / ("bad-root-" + str(len(str(bad)))), bazel_inputs=True)
                (staged / "extra.txt").write_bytes(b"extra")
                with self.assertRaisesRegex(ValueError, "declared Bazel tree"):
                    merge([relative], root / "extra", bazel_inputs=True)
                (staged / "extra.txt").unlink()
                (staged / "manifest.json").unlink()
                (staged / "manifest.json").symlink_to(root / "outside-manifest.json")
                (root / "outside-manifest.json").write_bytes((actual / "manifest.json").read_bytes())
                with self.assertRaisesRegex(ValueError, "declared Bazel tree"):
                    merge([relative], root / "manifest-escape", bazel_inputs=True)
            finally:
                os.chdir(previous)

    def test_bounds_and_disallowed_types(self):
        for identity, extension, payload in [("../escape", "svg", b"x"),
                                             ("kSvg", "js", b"x"), ("kSvg", "svg", b"")]:
            with self.subTest(identity=identity, extension=extension):
                with self.assertRaises(ValueError):
                    resource_record(identity, payload, extension, "chrome")


class ProductionGeneratorDependenciesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        package = Path(__file__).parent
        cls.build_source = (package / "BUILD.bazel").read_text()
        cls.module_source = (package.parent / "MODULE.bazel").read_text()
        cls.repositories = _generator_package_repositories(cls.build_source)

    def test_production_generator_rules_are_available_to_downstream_consumers(self):
        self.assertEqual(_production_generator_dependency_errors(
            self.module_source, self.build_source), [])

    def test_rejects_required_dependencies_marked_development_only(self):
        for repository in sorted(self.repositories):
            with self.subTest(repository=repository):
                changed = _mutate_module_dependency(self.module_source, repository, ast.Constant(True))
                self.assertEqual(_production_generator_dependency_errors(changed, self.build_source),
                                 ["Development-only production dependency: " + repository])

    def test_rejects_missing_required_dependencies(self):
        for repository in sorted(self.repositories):
            with self.subTest(repository=repository):
                changed = _mutate_module_dependency(self.module_source, repository, None)
                self.assertEqual(_production_generator_dependency_errors(changed, self.build_source),
                                 ["Missing production dependency: " + repository])

    def test_rejects_unknown_or_malformed_development_flags(self):
        flags = [ast.Name(id="unknown", ctx=ast.Load()), ast.Constant("False"),
                 ast.Constant(0), ast.Constant(None)]
        for repository in sorted(self.repositories):
            for flag in flags:
                with self.subTest(repository=repository, flag=ast.dump(flag)):
                    changed = _mutate_module_dependency(self.module_source, repository, flag)
                    with self.assertRaisesRegex(ValueError, "dev_dependency must be a literal boolean"):
                        _production_generator_dependency_errors(changed, self.build_source)

    def test_resolves_module_repository_aliases_without_executing_declarations(self):
        build_source = ('load("@generator_rules//:defs.bzl", "py_binary")\n'
                        'load("@other_rules//:defs.bzl", alias="rule")\n'
                        'load("//:local.bzl", "local_rule")\n'
                        'py_binary(name="packaged_resources_generator")\n')
        module_source = ('bazel_dep(name="generator_module", repo_name="generator_rules")\n'
                         'bazel_dep(name="other_rules", dev_dependency=False)\n'
                         'unresolved_call_that_must_not_execute()\n')
        self.assertEqual(_generator_package_repositories(build_source),
                         {"generator_rules", "other_rules"})
        self.assertEqual(_production_generator_dependency_errors(module_source, build_source), [])


if __name__ == "__main__":
    unittest.main()
