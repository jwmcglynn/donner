"""Tests for tools/check_private_details.py.

Fixture values with a private shape are assembled at runtime so that this file
itself passes the check without a waiver.
"""

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("check_private_details.py")
MODULE_SPEC = importlib.util.spec_from_file_location("check_private_details", MODULE_PATH)
assert MODULE_SPEC is not None
assert MODULE_SPEC.loader is not None
checker = importlib.util.module_from_spec(MODULE_SPEC)
sys.modules[MODULE_SPEC.name] = checker
MODULE_SPEC.loader.exec_module(checker)


def join(*parts):
    return "".join(parts)


MAC_HOME = join("/", "Users", "/")
LINUX_HOME = join("/", "home", "/")
WINDOWS_HOME = join("C:", "\\", "Users", "\\")
FAKE_NAMES = checker.PrivateNames.parse("exhost-alpha, quill.example\naccount:ziggy  # comment")


def kinds(text, names=checker.PrivateNames()):
    return [finding.kind for finding in checker.scan_text("file.txt", text, names)]


def ipv4(*octets):
    return ".".join(str(octet) for octet in octets)


class HomePathTests(unittest.TestCase):
    def test_flags_personal_home_directories(self):
        for text in [
            MAC_HOME + "alice/Projects/donner",
            "cd " + LINUX_HOME + "bob/src",
            '"' + MAC_HOME + 'carol/x"',
            "file://" + LINUX_HOME + "dave/x",
            WINDOWS_HOME + "erin\\src",
            join("c:/", "users/", "frank/x"),
        ]:
            with self.subTest(text=text):
                self.assertEqual(kinds(text), [checker.KIND_HOME_PATH])

    def test_allows_generic_system_and_sandbox_paths(self):
        for text in [
            LINUX_HOME + "user/art/a.svg",
            LINUX_HOME + "runner/work",
            LINUX_HOME + "fuzzer/.cache/bazel",
            LINUX_HOME + "linuxbrew/.linuxbrew/bin",
            MAC_HOME + "Shared/data",
            MAC_HOME + "username/Projects/donner",
            MAC_HOME + "<name>/Projects",
            "/private" + LINUX_HOME + "runfiles/app",
            "/path/to/donner",
        ]:
            with self.subTest(text=text):
                self.assertEqual(kinds(text), [])


class PrivateAddressTests(unittest.TestCase):
    def test_flags_private_ipv4_ranges(self):
        for text in [
            "grpc://" + ipv4(192, 168, 4, 20) + ":9092",
            ipv4(10, 1, 2, 3),
            "host " + ipv4(172, 20, 0, 5) + " up",
            ipv4(100, 64, 0, 1),
            ipv4(100, 127, 255, 254),
            ipv4(169, 254, 1, 1),
        ]:
            with self.subTest(text=text):
                self.assertEqual(kinds(text), [checker.KIND_PRIVATE_ADDRESS])

    def test_flags_ipv6_unique_local(self):
        for text in [join("fd12:", "3456:789a::1"), join("[FC00:", ":1]:443")]:
            with self.subTest(text=text):
                self.assertEqual(kinds(text), [checker.KIND_PRIVATE_ADDRESS])

    def test_ignores_public_addresses_and_versions(self):
        for text in [
            ipv4(8, 8, 8, 8),
            ipv4(172, 32, 0, 1),
            ipv4(100, 128, 0, 1),
            ipv4(127, 0, 0, 1),
            "version " + ipv4(10, 1, 2, 3),
            "SDK " + ipv4(10, 0, 2, 0),
            "v" + ipv4(10, 1, 2, 3),
            ipv4(10, 0, 19041, 1),
            ipv4(10, 1, 2, 3) + ".4",
            "fd_set fdcount: 3",
        ]:
            with self.subTest(text=text):
                self.assertEqual(kinds(text), [])


class PrivateSuffixTests(unittest.TestCase):
    def test_flags_private_suffix_hosts(self):
        for text in [
            join("printer", ".local"),
            join("grpcs://cache.", "lan:50051"),
            join("api.corp", ".internal/path"),
            join("router.HOME", ".ARPA"),
        ]:
            with self.subTest(text=text):
                self.assertEqual(kinds(text), [checker.KIND_PRIVATE_SUFFIX])

    def test_ignores_public_names_and_code(self):
        for text in [
            "internal.example.invalid",
            "donner::svg::internal",
            "localhost",
            "x.localdomain",
            "the local cache",
        ]:
            with self.subTest(text=text):
                self.assertEqual(kinds(text), [])


class PrivateNameTests(unittest.TestCase):
    def test_names_are_skipped_without_a_list(self):
        self.assertEqual(kinds("runs on exhost-alpha"), [])
        self.assertFalse(checker.PrivateNames())

    def test_flags_configured_names_in_any_spelling(self):
        for text in [
            "runs on exhost-alpha",
            "grpcs://exhost-alpha.example:50051",
            "EXHOST_ALPHA sandbox",
            "exhostalpha",
            "exhost alpha",
            "exhost.alpha",
            "build-exhost-alpha-worker",
            "see https://svc.quill.example/",
        ]:
            with self.subTest(text=text):
                self.assertEqual(kinds(text, FAKE_NAMES), [checker.KIND_PRIVATE_NAME])

    def test_ignores_partial_and_punctuated_matches(self):
        for text in ["exhost", "alpha", "exhost/alpha", "exhost: alpha", "exhostalphabet"]:
            with self.subTest(text=text):
                self.assertEqual(kinds(text, FAKE_NAMES), [])

    def test_exact_entries_match_only_as_written(self):
        names = checker.PrivateNames.parse("exact:box9")
        for text in ["on box9 today", "BOX9.example", "a-box9-b"]:
            with self.subTest(text=text):
                self.assertEqual(kinds(text, names), [checker.KIND_PRIVATE_NAME])
        for text in ["box-9", "box 9", "box90", "abox9"]:
            with self.subTest(text=text):
                self.assertEqual(kinds(text, names), [])

    def test_reports_one_name_finding_per_line(self):
        self.assertEqual(kinds("exhost-alpha and EXHOST-ALPHA", FAKE_NAMES), [checker.KIND_PRIVATE_NAME])

    def test_flags_configured_accounts_in_user_contexts(self):
        for text in ["User=ziggy", "cd ~ziggy/src", WINDOWS_HOME + "Ziggy\\x"]:
            with self.subTest(text=text):
                self.assertIn(checker.KIND_PRIVATE_ACCOUNT, kinds(text, FAKE_NAMES))
        self.assertEqual(kinds("TODO(ziggy): fix", FAKE_NAMES), [])


class LoadPrivateNamesTests(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)

    def tearDown(self):
        self.temporary_directory.cleanup()

    def test_absent_source_is_empty(self):
        self.assertFalse(checker.load_private_names(self.root, environ={}))

    def test_reads_default_file_env_file_and_env_value(self):
        (self.root / ".private-names").write_text("exhost-alpha\n", encoding="utf-8")
        self.assertEqual(checker.load_private_names(self.root, environ={}).names, {"exhostalpha"})
        other = self.root / "names.txt"
        other.write_text("account:ziggy\n", encoding="utf-8")
        loaded = checker.load_private_names(self.root, environ={checker.NAMES_FILE_ENV: str(other)})
        self.assertEqual(loaded.accounts, {"ziggy"})
        loaded = checker.load_private_names(self.root, environ={checker.NAMES_ENV: "quill.example"})
        self.assertEqual(loaded.names, {"quillexample"})


class ScanFilesTests(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)

    def tearDown(self):
        self.temporary_directory.cleanup()

    def write(self, relative, content):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(content, bytes):
            path.write_bytes(content)
        else:
            path.write_text(content, encoding="utf-8")

    def test_reports_location_without_echoing_the_value(self):
        self.write("docs/a.md", "ok\nsee " + MAC_HOME + "alice/notes\n")
        findings = checker.scan_files(self.root, ["docs/a.md"])
        self.assertEqual([str(finding) for finding in findings],
                         ["docs/a.md:2: home-path: personal home directory path"])
        self.assertNotIn("alice", str(findings[0]))

    def test_skips_binary_and_missing_files(self):
        self.write("image.png", b"\x89PNG\0" + (MAC_HOME + "alice").encode())
        self.assertEqual(checker.scan_files(self.root, ["image.png", "missing.txt"]), [])

    def test_main_exit_status(self):
        self.write("clean.txt", "nothing private here\n")
        self.write("dirty.txt", "cache at " + ipv4(10, 0, 0, 7) + "\n")
        self.assertEqual(checker.main(["--root", str(self.root), "clean.txt"]), 0)
        self.assertEqual(checker.main(["--root", str(self.root), "dirty.txt"]), 1)


if __name__ == "__main__":
    unittest.main()
