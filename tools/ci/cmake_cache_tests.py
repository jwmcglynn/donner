"""Checks compiler identity, cached source integrity, and cache-independent execution."""
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock

from python.runfiles import runfiles

resolver = runfiles.Create()
spec = importlib.util.spec_from_file_location("cmake_cache", resolver.Rlocation("donner/tools/ci/cmake_cache.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def archive(files):
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w:gz") as output:
        for name, content in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(content)
            info.mode = 0o644
            output.addfile(info, io.BytesIO(content))
    return data.getvalue()


class CMakeCacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_qualification_rejects_disabled_skipped_cold_or_mismatched_evidence(self):
        receipt = {"persistent": True, "enabled": True, "cacheable_calls": 100, "tests": 2,
                   "test_failures": 0, "test_skips": 0, "hits": 0, "misses": 100, "source_hits": 0,
                   "checkout_sha": "a" * 40, "head_sha": "b" * 40, "run_id": "1", "run_attempt": "1"}
        cold = self.root / "producer.json"
        cold.write_text(json.dumps(receipt))
        m.qualify(cold, "producer")
        warm = dict(receipt, hits=99, misses=1, source_hits=8)
        path = self.root / "consumer.json"
        path.write_text(json.dumps(warm))
        m.qualify(path, "consumer", cold)
        for change in ({"persistent": False}, {"tests": 0}, {"test_skips": 1}, {"hits": 0}, {"run_attempt": "2"}):
            path.write_text(json.dumps(dict(warm, **change)))
            with self.subTest(change=change), self.assertRaises(m.CacheError):
                m.qualify(path, "consumer", cold)

    def test_concurrent_downloads_produce_readable_entries(self):
        from concurrent.futures import ThreadPoolExecutor
        descriptor = {"url": "https://github.com/a/b/archive/v1.tar.gz"}
        body = archive({"root/header.h": b"content"})
        with mock.patch.object(m, "download", return_value=body), ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda _: m.cached_archive(self.root, descriptor), range(12)))
            self.assertTrue(all(result[0] == body for result in results))
            self.assertEqual((body, True), m.cached_archive(self.root, descriptor))
        self.assertEqual([], list(self.root.glob("*.tmp-*")))

    def test_download_eviction_keeps_newer_owned_entries_and_unknown_files(self):
        old, new = self.root / ("a" * 64 + ".tar"), self.root / ("b" * 64 + ".tar")
        old.write_bytes(b"12345"); new.write_bytes(b"67890")
        os.utime(old, (1, 1)); os.utime(new, (2, 2))
        unrelated = self.root / "unmanaged.tar"; unrelated.write_bytes(b"preserve")
        with mock.patch.object(m, "DOWNLOAD_BUDGET", 5):
            m.trim_downloads(self.root)
        self.assertFalse(old.exists())
        self.assertEqual(b"67890", new.read_bytes())
        self.assertEqual(b"preserve", unrelated.read_bytes())

    def test_tool_download_is_digest_pinned_before_execution(self):
        with mock.patch.object(m, "download", return_value=archive({"package/ccache": b"untrusted"})):
            with self.assertRaisesRegex(m.CacheError, "checksum mismatch"):
                m.install_tool(self.root)
        self.assertFalse((self.root / "ccache").exists())

    def test_tool_archive_does_not_follow_a_symlink(self):
        data = io.BytesIO()
        with tarfile.open(fileobj=data, mode="w:gz") as output:
            info = tarfile.TarInfo("package/ccache")
            info.type = tarfile.SYMTYPE
            info.linkname = "/bin/sh"
            output.addfile(info)
        body = data.getvalue()
        key = (m.platform.system(), m.platform.machine())
        with mock.patch.dict(m.ARTIFACTS, {key: ("fixture.tar.gz", m.digest(body))}), \
             mock.patch.object(m, "download", return_value=body):
            with self.assertRaisesRegex(m.CacheError, "executable archive"):
                m.install_tool(self.root)

    def test_cache_corruption_fetches_verified_data_again(self):
        descriptor = {"kind": "archive", "url": "https://github.com/a/b/archive/v1.tar.gz"}
        body = archive({"root/header.h": b"original"})
        with mock.patch.object(m, "download", return_value=body) as fetch:
            self.assertEqual((body, False), m.cached_archive(self.root, descriptor))
            self.assertEqual((body, True), m.cached_archive(self.root, descriptor))
            path = next(self.root.glob("*.tar"))
            path.write_bytes(b"corrupted")
            self.assertEqual((body, False), m.cached_archive(self.root, descriptor))
        self.assertEqual(2, fetch.call_count)

    def test_declared_archive_digest_is_enforced(self):
        descriptor = {"kind": "archive", "url": "https://github.com/a/b/archive/v1.tar.gz", "sha256": "0" * 64}
        with mock.patch.object(m, "download", return_value=b"wrong"):
            with self.assertRaisesRegex(m.CacheError, "checksum mismatch"):
                m.cached_archive(self.root, descriptor)
        self.assertEqual([], list(self.root.iterdir()))

    def test_changed_dependency_revision_selects_a_different_entry(self):
        one = {"url": "https://github.com/a/b/archive/v1.tar.gz", "revision": "v1"}
        two = dict(one, revision="v2")
        body = archive({"root/a": b"data"})
        with mock.patch.object(m, "download", return_value=body) as fetch:
            m.cached_archive(self.root, one)
            m.cached_archive(self.root, two)
        self.assertEqual(2, fetch.call_count)
        self.assertEqual(2, len(list(self.root.glob("*.tar"))))

    def test_only_qualified_complete_declarations_override_fetchcontent(self):
        revision = "a" * 40
        plain = "GIT_REPOSITORY https://github.com/google/googletest.git\n GIT_TAG " + revision
        for extra in ("", "\n GIT_SUBMODULES child", "\n PATCH_COMMAND tool", "\n SOURCE_SUBDIR child"):
            (self.root / "CMakeLists.txt").write_text("FetchContent_Declare(googletest\n " + plain + extra + "\n)\n")
            self.assertEqual(1 if not extra else 0, len(list(m.source_declarations(self.root))))
        (self.root / "CMakeLists.txt").write_text("FetchContent_Declare(unknown\n " + plain + "\n)\n")
        self.assertEqual([], list(m.source_declarations(self.root)))

    def test_source_descriptor_requires_immutable_commit(self):
        with self.assertRaises(m.CacheError):
            m.source_descriptor({"kind": "git", "repository": "https://github.com/a/b.git", "revision": "v1"})

    def test_source_extraction_rejects_traversal_and_links(self):
        for name in ("root/../../outside", "/outside"):
            with self.subTest(name=name), self.assertRaises(m.CacheError):
                m.extract_sources(archive({name: b"data"}), self.root / str(len(name)))
        self.assertFalse((self.root.parent / "outside").exists())

    def test_existing_extracted_sources_are_rehashed(self):
        body = archive({"root/header.h": b"correct"})
        destination = self.root / "source"
        m.extract_sources(body, destination)
        m.verify_sources(body, destination)
        (destination / "header.h").write_bytes(b"changed")
        with self.assertRaisesRegex(m.CacheError, "source changed"):
            m.verify_sources(body, destination)

    def test_existing_source_parent_symlink_is_rejected(self):
        body = archive({"root/sub/header.h": b"correct"})
        destination = self.root / "source"
        destination.mkdir()
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "header.h").write_bytes(b"correct")
        (destination / "sub").symlink_to(outside)
        with self.assertRaises(m.CacheError):
            m.verify_sources(body, destination)

    def test_url_redirect_cannot_leave_known_https_origins(self):
        for url in ("http://github.com/a", "https://evil.test/a", "https://u:p@github.com/a"):
            with self.subTest(url=url), self.assertRaises(m.CacheError):
                m.validate_url(url)
        m.validate_url("https://codeload.github.com/a/b/tar.gz/commit")

    def test_nix_wrapper_and_sdk_environment_are_identity_inputs(self):
        env = {"NIX_CFLAGS_COMPILE_aarch64_unknown_linux_gnu": "-DVALUE=1", "SDKROOT": "sdk", "NIX_HARDENING_ENABLE": "fortify", "GITHUB_TOKEN": "secret", "GITHUB_SHA": "head"}
        result = m.relevant_environment(env)
        self.assertEqual({"NIX_CFLAGS_COMPILE_aarch64_unknown_linux_gnu": "-DVALUE=1", "SDKROOT": "sdk", "NIX_HARDENING_ENABLE": "fortify"}, result)
        changed = dict(result, NIX_CFLAGS_COMPILE_aarch64_unknown_linux_gnu="-DVALUE=2")
        self.assertNotEqual(m.digest(m.canonical(result)), m.digest(m.canonical(changed)))

    def test_source_revision_does_not_invalidate_all_compiler_entries(self):
        identity = {"content": "compiler"}
        with mock.patch.object(m, "compiler_identity", return_value=identity), mock.patch.object(m, "sdk_identity", return_value={}):
            _, one = m.fingerprint(self.root, {"GITHUB_SHA": "one"})
            _, two = m.fingerprint(self.root, {"GITHUB_SHA": "two"})
        self.assertEqual(one, two)
        with mock.patch.object(m, "compiler_identity", return_value=identity), mock.patch.object(m, "sdk_identity", return_value={}):
            value, three = m.fingerprint(self.root, {"NIX_CONFIG": "access-tokens=secret"})
        self.assertNotEqual(one, three)
        self.assertNotIn("secret", json.dumps(value))

    def test_configure_receives_launchers_but_no_shared_configuration(self):
        context = self.root / "context.json"
        value = {"schema": 1, "tool": "/private/tool/ccache", "environment": {"CCACHE_COMPILERCHECK": "content"}, "configure": ["-DFETCHCONTENT_SOURCE_DIR_A=/fresh/a"]}
        context.write_text(json.dumps(value))
        with mock.patch.dict(os.environ, {"CCACHE_SLOPPINESS": "time_macros", "CCACHE_REMOTE_STORAGE": "untrusted"}), mock.patch.object(m.subprocess, "call", return_value=0) as call:
            self.assertEqual(0, m.run_context(context, ["cmake", "-S", "source"], True))
        args = call.call_args.args[0]
        env = call.call_args.kwargs["env"]
        self.assertIn("-DCMAKE_CXX_COMPILER_LAUNCHER=/private/tool/ccache", args)
        self.assertNotIn("CCACHE_SLOPPINESS", env)
        self.assertNotIn("CCACHE_REMOTE_STORAGE", env)

    def test_unavailable_cache_can_compile_without_a_launcher(self):
        context = self.root / "context.json"
        context.write_text(json.dumps({"schema": 1, "tool": None, "environment": {}, "configure": []}))
        with mock.patch.object(m.subprocess, "call", return_value=0) as call:
            self.assertEqual(0, m.run_context(context, ["cmake", "-S", "source"], True))
        self.assertIn("-DCMAKE_CXX_COMPILER_LAUNCHER=", call.call_args.args[0])


if __name__ == "__main__":
    unittest.main()
