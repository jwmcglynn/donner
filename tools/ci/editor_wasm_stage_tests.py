"""Runs the Editor WASM staging script against synthetic packages and a recording Bazel stand-in."""

import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

from python.runfiles import runfiles
from tools import test_timing
from tools.test_executable_fixtures import ExecutableFixtureDirectory

SHA = "a" * 40
PACKAGE_TARGET = "//fixture:package"
# The bazelisk commands the handoff step runs, in order.
PACKAGE_QUERIES = [
    ["query", "--output=label", "labels(actual, //tools/ci:editor_wasm_package)"],
    ["cquery", "--config=editor-wasm", "--//build_defs:release_source_commit=" + SHA,
     "--output=files", "//tools/ci:editor_wasm_package"],
]
FAMILIES = ("Bebas Neue", "Bitter", "Inter", "JetBrains Mono", "Lato", "Lora", "Montserrat",
            "Open Sans", "Oswald", "Pacifico", "Playfair Display", "Roboto Mono")


def _files(directory):
    return {path.relative_to(directory).as_posix(): path.read_bytes()
            for path in directory.rglob("*") if path.is_file()}


class EditorWasmStageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        resolver = runfiles.Create()
        cls.script = Path(resolver.Rlocation("donner/tools/ci/editor_wasm_stage.py"))
        cls.integrity = Path(resolver.Rlocation(
            "donner/donner/editor/wasm/catalog_package_integrity_test.py"))
        cls.executables = ExecutableFixtureDirectory()
        cls.addClassCleanup(cls.executables.cleanup)
        # Workflow scripts call a bare python3; it must be the test's own runtime.
        cls.executables.write_python3()
        # Records each bazelisk command, then answers the handoff's two queries from the
        # environment or exits with BAZEL_STATUS.
        (cls.executables.path / "bazelisk.py").write_text(
            "import json, os, sys\n"
            "with open(os.environ['CAPTURE'], 'a', encoding='utf-8') as capture:\n"
            "    capture.write(json.dumps([sys.argv[1:], os.getcwd()]) + '\\n')\n"
            "status = int(os.environ.get('BAZEL_STATUS', '0'))\n"
            "if status:\n"
            "    sys.exit(status)\n"
            "print(os.environ['QUERY_LABEL'] if sys.argv[1] == 'query' "
            "else os.environ['PACKAGE_PATH'])\n",
            encoding="utf-8",
        )
        cls.executables.write_python_entry("bazelisk", cls.executables.path / "bazelisk.py")
        # Checksums are computed in process; a per-file hashing process would cost a process
        # start for each of the package's files.
        cls.executables.write("shasum", "#!/bin/sh\necho shasum must not run >&2\nexit 97\n")

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="editor wasm stage ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.repo = self.root / "repo"
        (self.repo / "tools/ci").mkdir(parents=True)
        shutil.copy(self.script, self.repo / "tools/ci/editor_wasm_stage.py")
        wasm = self.repo / "donner/editor/wasm"
        (wasm / "tests").mkdir(parents=True)
        shutil.copy(self.integrity, wasm / "catalog_package_integrity_test.py")
        (wasm / "tests/package-lock.json").write_text("{}")
        self.runner_temp = self.root / "runner temp"
        self.runner_temp.mkdir()
        self.output = self.root / "github output"
        self.summary = self.root / "step summary"
        self.capture = self.root / "bazel calls.jsonl"
        self.capture.write_text("")

    def _run(self, command, **environment):
        env = {
            "PATH": os.pathsep.join((str(self.executables.path), os.environ["PATH"])),
            "RUNNER_TEMP": str(self.runner_temp), "GITHUB_OUTPUT": str(self.output),
            "GITHUB_STEP_SUMMARY": str(self.summary), "GITHUB_SHA": SHA,
            "GITHUB_RUN_ID": "12", "GITHUB_RUN_ATTEMPT": "2", "PACKAGE_TARGET": PACKAGE_TARGET,
            "CAPTURE": str(self.capture), "QUERY_LABEL": PACKAGE_TARGET,
            **environment,
        }
        env = {name: value for name, value in env.items() if value is not None}
        return subprocess.run(
            [sys.executable, str(self.repo / "tools/ci/editor_wasm_stage.py"), command],
            cwd=self.root, env=env, capture_output=True, text=True, check=False,
            timeout=test_timing.STALL_GUARD_SECONDS)

    def _bazel_calls(self):
        calls = []
        for line in self.capture.read_text().splitlines():
            command, cwd = json.loads(line)
            self.assertEqual(cwd, str(self.repo.resolve()), command)
            calls.append(command)
        return calls

    # The package handoff.

    def test_handoff_resolves_the_package_and_copies_it_through_links(self):
        package = self.root / "bazel-out/package"
        (package / "fonts").mkdir(parents=True)
        (package / "editor.wasm").write_bytes(b"package fixture")
        (self.root / "font.woff2").write_bytes(b"linked font")
        (package / "fonts/font.woff2").symlink_to(self.root / "font.woff2")
        result = self._run("package", PACKAGE_PATH=str(package))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self._bazel_calls(), PACKAGE_QUERIES)
        handoff = self.runner_temp / "editor-wasm-package-geode"
        self.assertEqual(self.output.read_text(),
                         "package_target=%s\nhandoff_dir=%s\n" % (PACKAGE_TARGET, handoff))
        self.assertEqual(_files(handoff), {"editor.wasm": b"package fixture",
                                           "fonts/font.woff2": b"linked font"})
        self.assertFalse((handoff / "fonts/font.woff2").is_symlink())

    def test_handoff_configuration_uses_the_selected_source_revision(self):
        package = self.root / "package"
        package.mkdir()
        for revision in ("a" * 40, "b" * 40):
            result = self._run("package", PACKAGE_PATH=str(package), GITHUB_SHA=revision)
            self.assertEqual(result.returncode, 0, result.stderr)
            query = self._bazel_calls()[-1]
            self.assertIn("--//build_defs:release_source_commit=" + revision, query)
            self.capture.write_text("")

    def test_handoff_without_ci_context_keeps_the_local_configuration(self):
        package = self.root / "package"
        package.mkdir()
        result = self._run("package", PACKAGE_PATH=str(package), GITHUB_SHA=None)
        self.assertEqual(result.returncode, 0, result.stderr)
        local = [[word for word in call if not word.startswith("--//build_defs:release_source_commit=")]
                 for call in PACKAGE_QUERIES]
        self.assertEqual(self._bazel_calls(), local)

    def test_handoff_invalid_source_revision_fails_before_bazel(self):
        package = self.root / "package"
        package.mkdir()
        for revision in ("", "short", "A" * 40):
            self.capture.write_text("")
            result = self._run("package", PACKAGE_PATH=str(package), GITHUB_SHA=revision)
            self.assertNotEqual(result.returncode, 0, revision)
            self.assertEqual(self._bazel_calls(), [], revision)

    def test_handoff_rejects_an_ambiguous_missing_or_unexpected_package(self):
        package = self.root / "package"
        package.mkdir()
        (package / "editor.wasm").write_bytes(b"package fixture")
        cases = {
            "ambiguous outputs": ({"PACKAGE_PATH": "%s\n%s" % (package, package)},
                                  "package_target=%s\n" % PACKAGE_TARGET),
            "no output": ({"PACKAGE_PATH": ""}, "package_target=%s\n" % PACKAGE_TARGET),
            "output is not a directory": ({"PACKAGE_PATH": str(package / "editor.wasm")},
                                          "package_target=%s\n" % PACKAGE_TARGET),
            "malformed label": ({"PACKAGE_PATH": str(package),
                                 "QUERY_LABEL": "//fixture:package extra"}, ""),
            "label with newline": ({"PACKAGE_PATH": str(package),
                                    "QUERY_LABEL": "//a:b\n//c:d"}, ""),
        }
        for name, (environment, written) in cases.items():
            with self.subTest(case=name):
                self.output.write_text("")
                result = self._run("package", **environment)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(self.output.read_text(), written)
                self.assertFalse((self.runner_temp / "editor-wasm-package-geode").exists())

    def test_handoff_stops_with_the_failing_bazel_status(self):
        result = self._run("package", PACKAGE_PATH=str(self.root), BAZEL_STATUS="7")
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(self._bazel_calls(), PACKAGE_QUERIES[:1])
        self.assertFalse(self.output.exists())

    # The deployment candidate.

    def _candidate_fixture(self):
        package = self.runner_temp / "editor-wasm-package-geode"
        (package / "fonts").mkdir(parents=True)
        for name in ("donner_icon.svg", "editor-bootstrap.js", "editor.css", "editor.js",
                     "enable-threads.js", "index.html", "catalog-fonts.js"):
            (package / name).write_text(name)
        (package / "editor.wasm").write_bytes(b"\0asm\1\0\0\0")
        (package / "CatalogFontNotices.txt").write_text("SIL OPEN FONT LICENSE Version 1.1")
        fonts = []
        for index, family in enumerate(FAMILIES):
            payload = bytearray([index] * 64)
            payload[:4] = b"wOF2"
            struct.pack_into(">I", payload, 16, 128)
            digest = hashlib.sha256(payload).hexdigest()
            path = "fonts/%s.woff2" % digest
            (package / path).write_bytes(payload)
            fonts.append({"family": family, "sha256": digest, "path": path,
                          "encoded_bytes": len(payload), "decoded_bytes": 128})
        (package / "catalog-fonts.json").write_text(json.dumps({
            "fonts": fonts, "encoded_bytes": 64 * len(fonts),
        }))
        # The proved MODULE.bazel.lock that the module-graph step snapshots into RUNNER_TEMP.
        (self.runner_temp / "editor-wasm-module.lock").write_text('{"lockFileVersion": 26}\n')
        return package

    def test_candidate_preserves_deferred_catalog_assets(self):
        package = self._candidate_fixture()
        expected = _files(package)
        result = self._run("candidate")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        candidate = self.runner_temp / ("donner-editor-wasm-geode-" + SHA)
        self.assertEqual(self.output.read_text(), "candidate_dir=%s\n" % candidate)
        self.assertEqual(_files(candidate / "site"), expected)
        deploy = _files(candidate / "deploy")
        compressed = deploy.pop("editor.wasm.gz")
        self.assertEqual(gzip.decompress(compressed), expected.pop("editor.wasm"))
        # gzip -n: no name and no timestamp, so the bytes depend only on the module.
        self.assertEqual(compressed[3] & 0x08, 0)
        self.assertEqual(compressed[4:8], b"\0\0\0\0")
        self.assertEqual(deploy, expected)
        module_lock = (self.runner_temp / "editor-wasm-module.lock").read_bytes()
        # The candidate retains the proved module lock outside the served trees.
        self.assertEqual((candidate / "MODULE.bazel.lock").read_bytes(), module_lock)
        self.assertEqual(
            (candidate / "provenance.json").read_bytes(),
            ('{"source_revision":"%s","producer_run_id":"12","producer_attempt":"2",'
             '"lockfile_sha256":"%s","generated_lock_sha256":"%s",'
             '"bazel_configs":["editor-wasm"],"targets":["%s"],'
             '"renderer":"geode","transport_encoding":"gzip"}\n' % (
                 SHA, hashlib.sha256(b"{}").hexdigest(), hashlib.sha256(module_lock).hexdigest(),
                 PACKAGE_TARGET)).encode())
        self.assertEqual(
            sorted(path.name for path in candidate.iterdir()),
            ["MODULE.bazel.lock", "SHA256SUMS", "deploy", "provenance.json", "site"])

    def test_candidate_manifest_lists_every_served_file_in_byte_order(self):
        self._candidate_fixture()
        result = self._run("candidate")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        candidate = self.runner_temp / ("donner-editor-wasm-geode-" + SHA)
        paths = sorted((path.relative_to(candidate).as_posix()
                        for directory in ("site", "deploy")
                        for path in (candidate / directory).rglob("*") if path.is_file()),
                       key=str.encode)
        self.assertEqual(
            (candidate / "SHA256SUMS").read_text(),
            "".join("%s  %s\n" % (hashlib.sha256((candidate / path).read_bytes()).hexdigest(),
                                  path) for path in paths))
        self.assertIn("deploy/CatalogFontNotices.txt", paths)
        self.assertLess(paths.index("deploy/CatalogFontNotices.txt"),
                        paths.index("deploy/catalog-fonts.js"))

    def test_candidate_rejects_incomplete_unexpected_or_oversized_assets(self):
        def corrupt_font(package):
            next((package / "fonts").glob("*.woff2")).write_bytes(b"corrupt")

        def oversized_module(package):
            # Incompressible, so the encoded module exceeds the 20 MiB transport limit.
            (package / "editor.wasm").write_bytes(
                b"\0asm\1\0\0\0" + os.urandom(20 * 1024 * 1024 + 4096))

        failures = {
            "corrupt-font": corrupt_font,
            "extra-font": lambda package: (package / "fonts/extra.woff2").write_bytes(b"x"),
            "extra-root-file": lambda package: (package / "unexpected.txt").write_text("x"),
            "missing-broker": lambda package: (package / "catalog-fonts.js").unlink(),
            "missing-module-lock":
                lambda package: (package.parent / "editor-wasm-module.lock").unlink(),
            "oversized-module": oversized_module,
        }
        for failure, damage in failures.items():
            with self.subTest(failure=failure):
                self.setUp()
                damage(self._candidate_fixture())
                result = self._run("candidate")
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse(self.output.exists())

    def test_candidate_requires_its_run_context(self):
        self._candidate_fixture()
        result = self._run("candidate", PACKAGE_TARGET=None)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("PACKAGE_TARGET", result.stderr)
        self.assertFalse(self.output.exists())

    # The job summary.

    def test_record_digest_appends_the_candidate_identity_to_the_summary(self):
        lock = b'{"lockFileVersion": 26}\n'
        (self.runner_temp / "editor-wasm-module.lock").write_bytes(lock)
        self.summary.write_text("earlier\n")
        result = self._run("record-digest", ARTIFACT_DIGEST="sha256:" + "b" * 64)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.summary.read_text(), (
            "earlier\n"
            "### Geode deployment candidate\n"
            "- Source revision: `%s`\n"
            "- Artifact digest: `sha256:%s`\n"
            "- Generated MODULE.bazel.lock SHA-256: `%s`\n") % (
                SHA, "b" * 64, hashlib.sha256(lock).hexdigest()))

    def test_unknown_subcommand_is_rejected(self):
        result = self._run("publish")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(self._bazel_calls(), [])


if __name__ == "__main__":
    unittest.main()
