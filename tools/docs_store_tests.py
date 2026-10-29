"""Immutable storage, corrupted chunk and concurrent-activation negative controls."""
import copy
import io
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from tools import docs_site as site
from tools import docs_store as store


class StoreTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.archive = self.root / "site.tar"
        self.archive.write_bytes(b"independent main and release bytes" * 500)
        self.payload = self.root / "payload"
        self.payload.mkdir()

    def test_roundtrip_survives_actions_expiry_without_github_artifacts(self):
        record = store.split_archive(self.archive, self.payload)
        output = self.root / "restored.tar"
        store.restore(record, output, lambda name: (self.payload / name).read_bytes())
        self.assertEqual(output.read_bytes(), self.archive.read_bytes())

    def test_github_raw_reads_preserve_binary_archive_chunks(self):
        record = store.split_archive(self.archive, self.payload)
        def run(args, *, stdout, **kwargs):
            self.assertIn("Accept: application/vnd.github.raw", args)
            self.assertNotIn("Accept: application/vnd.github.raw+json", args)
            path = args[2].split("/contents/", 1)[1].split("?ref=", 1)[0]
            stdout.write((self.payload / path).read_bytes())
            return store.subprocess.CompletedProcess(args, 0, stderr=b"")
        output = self.root / "downloaded.tar"
        with mock.patch.object(store.subprocess, "run", side_effect=run):
            store.restore(record, output, lambda name: store.GitHub().file("a" * 40, name))
        self.assertEqual(output.read_bytes(), self.archive.read_bytes())

    def test_corrupted_missing_and_oversized_chunks_are_rejected(self):
        record = store.split_archive(self.archive, self.payload)
        for failure in (b"changed", b"", b"x" * (store.CHUNK + 1)):
            with self.subTest(size=len(failure)), self.assertRaisesRegex(ValueError, "chunk changed"):
                store.restore(record, self.root / ("bad-" + str(len(failure)) + ".tar"), lambda name: failure)

    def test_uncompressed_size_and_digest_are_enforced(self):
        record = store.split_archive(self.archive, self.payload)
        for change in ({"size": 10}, {"sha256": "0" * 64}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                store.restore({**record, **change}, self.root / "bad.tar", lambda name: (self.payload / name).read_bytes())
            (self.root / "bad.tar").unlink(missing_ok=True)

    def test_chunk_path_cannot_select_another_blob(self):
        record = store.split_archive(self.archive, self.payload)
        record["chunks"][0]["path"] = "../secret"
        with self.assertRaisesRegex(ValueError, "invalid stored chunk"):
            store.validate_archive(record)

    def test_legacy_complete_archives_remain_restorable(self):
        import gzip
        data = b"previously published complete site"
        compressed = gzip.compress(data, mtime=1)
        digest = site.digest(compressed)
        part = {"path": f"objects/{digest}/0000.part", "sha256": digest, "size": len(compressed)}
        record = {"sha256": site.digest(data), "size": len(data), "compressed_sha256": digest,
                  "chunks": [part]}
        output = self.root / "legacy.tar"
        store.restore(record, output, lambda name: compressed)
        self.assertEqual(output.read_bytes(), data)

    def test_unchanged_raw_chunks_are_reused_across_complete_archives(self):
        one = self.root / "one.tar"
        two = self.root / "two.tar"
        one.write_bytes(b"s" * 1024 + b"old main" * 50)
        two.write_bytes(b"s" * 1024 + b"new main" * 50)
        with mock.patch.object(store, "RAW_CHUNK", 1024):
            first = store.split_archive(one, self.payload)
            second = store.split_archive(two, self.payload)
            self.assertEqual(first["schema"], 2)
            self.assertEqual(first["chunks"][0], second["chunks"][0])
            self.assertNotEqual(first["chunks"][1], second["chunks"][1])
            self.assertEqual(len(list(self.payload.rglob("*.part"))), 3)
            for name, record, original in (("first", first, one), ("second", second, two)):
                output = self.root / name
                store.restore(record, output, lambda path: (self.payload / path).read_bytes())
                self.assertEqual(output.read_bytes(), original.read_bytes())

    def test_compressed_raw_and_complete_hashes_are_independently_verified(self):
        record = store.split_archive(self.archive, self.payload)
        wrong_raw = copy.deepcopy(record)
        wrong_raw["chunks"][0]["raw_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "raw chunk changed"):
            store.restore(wrong_raw, self.root / "raw-bad", lambda path: (self.payload / path).read_bytes())
        wrong_total = {**record, "sha256": "0" * 64}
        with self.assertRaisesRegex(ValueError, "restored archive changed"):
            store.restore(wrong_total, self.root / "total-bad", lambda path: (self.payload / path).read_bytes())
        with self.assertRaisesRegex(ValueError, "format"):
            store.validate_archive({**record, "schema": 3})

    def test_chunk_expansion_is_bounded_by_its_declared_raw_size(self):
        record = store.split_archive(self.archive, self.payload)
        record["size"] = 1
        record["chunks"][0]["raw_size"] = 1
        with self.assertRaisesRegex(ValueError, "raw chunk changed"):
            store.restore(record, self.root / "expansion", lambda path: (self.payload / path).read_bytes())

    def test_pinned_branch_advance_blocks_before_any_blob_write(self):
        api = mock.Mock()
        api.tip.return_value = "b" * 40
        with self.assertRaisesRegex(ValueError, "advanced"):
            store.commit(api, "a" * 40, self.payload, {})
        api.call.assert_not_called()

    def test_existing_immutable_entry_cannot_be_replaced(self):
        (self.payload / "objects").mkdir()
        (self.payload / "objects/x").write_bytes(b"new")
        api = mock.Mock()
        api.tip.return_value = "a" * 40
        api.file.return_value = b"old"
        with self.assertRaisesRegex(ValueError, "immutable storage entry"):
            store.commit(api, "a" * 40, self.payload, {})
        api.call.assert_not_called()

    def test_failure_reading_old_entry_does_not_create_or_overwrite_it(self):
        (self.payload / "objects").mkdir()
        (self.payload / "objects/x").write_bytes(b"new")
        api = mock.Mock()
        api.tip.return_value = "a" * 40
        api.file.side_effect = ValueError("permission denied")
        with self.assertRaisesRegex(ValueError, "cannot inspect"):
            store.commit(api, "a" * 40, self.payload, {})
        api.call.assert_not_called()

    def test_rate_limited_request_is_repeated_after_the_reset_with_fresh_output(self):
        import json
        import time
        calls = []
        def run(args, *, stdout, **kwargs):
            calls.append(args[:3])
            if args[:3] == ["gh", "api", "rate_limit"]:
                window = {"limit": 1000, "remaining": 0, "used": 1000, "reset": int(time.time())}
                return store.subprocess.CompletedProcess(
                    args, 0, json.dumps({"resources": {"core": window}}), "")
            if len(calls) == 1:
                stdout.write(b'{"partial"')
                return store.subprocess.CompletedProcess(
                    args, 1, stderr=b"HTTP 403: API rate limit exceeded for installation.")
            stdout.write(b'{"object": {"sha": "' + b"b" * 40 + b'"}}')
            return store.subprocess.CompletedProcess(args, 0, stderr=b"")
        with mock.patch.object(store.subprocess, "run", side_effect=run):
            self.assertEqual(store.GitHub().tip(), "b" * 40)
        endpoint = ["gh", "api", "repos/jwmcglynn/donner/git/ref/heads/docs-snapshots"]
        self.assertEqual(calls, [endpoint, ["gh", "api", "rate_limit"], endpoint])

    def test_actual_subprocess_timeout_becomes_an_unknown_outcome(self):
        import subprocess
        with mock.patch.object(store.subprocess, "run", side_effect=subprocess.TimeoutExpired("gh", 180)), \
             self.assertRaisesRegex(ValueError, "outcome is unknown"):
            store.GitHub().call("repos/jwmcglynn/donner/pages/deployments", "POST", {"artifact_id": 1})

    def test_writer_requires_complete_live_branch_and_environment_protection(self):
        rules = [{"type": "deletion"}, {"type": "non_fast_forward"}]
        env = {"can_admins_bypass": False, "protection_rules": [
            {"type": "required_reviewers", "prevent_self_review": False, "reviewers": [
                {"type": "User", "reviewer": {"login": "jwmcglynn", "id": 103489}}]}],
            "deployment_branch_policy": {"protected_branches": False, "custom_branch_policies": True}}
        branches = {"total_count": 1, "branch_policies": [{"name": "main", "type": "branch"}]}
        api = store.GitHub()
        with mock.patch.object(api, "call", side_effect=[rules, env, branches]):
            api.require_protection()
        with mock.patch.object(api, "call", side_effect=[rules, {**env, "can_admins_bypass": True}, branches]):
            api.require_protection()
        for changed in ({**env, "can_admins_bypass": None}, {**env, "protection_rules": []}):
            with mock.patch.object(api, "call", side_effect=[rules, changed]), self.assertRaisesRegex(ValueError, "required reviewers"):
                api.require_protection()
        with mock.patch.object(api, "call", side_effect=[rules, env,
            {"total_count": 1, "branch_policies": [{"name": "*", "type": "branch"}]}]), \
            self.assertRaisesRegex(ValueError, "only the main"):
            api.require_protection()

    def test_manual_environment_rejects_another_reviewer_or_team(self):
        rules = [{"type": "deletion"}, {"type": "non_fast_forward"}]
        reviewer = {"type": "User", "reviewer": {"login": "jwmcglynn", "id": 103489}}
        env = {"can_admins_bypass": True, "protection_rules": [
            {"type": "required_reviewers", "prevent_self_review": False, "reviewers": [reviewer]}],
            "deployment_branch_policy": {"protected_branches": False, "custom_branch_policies": True}}
        for replacement in ([{"type": "User"}], [{"type": "Team", "reviewer": {"login": "jwmcglynn", "id": 103489}}],
                            [{"type": "User", "reviewer": {"login": "another-user", "id": 103489}}],
                            [{"type": "User", "reviewer": {"login": "jwmcglynn", "id": 1}}], [reviewer, reviewer]):
            changed = copy.deepcopy(env)
            changed["protection_rules"][0]["reviewers"] = replacement
            with self.subTest(replacement=replacement), mock.patch.object(store.GitHub, "call", side_effect=[rules, changed, {"total_count": 1, "branch_policies": [{"name": "main", "type": "branch"}]}]), \
                 self.assertRaisesRegex(ValueError, "reviewer"):
                store.GitHub().require_protection()

    def test_automatic_environment_has_its_own_exact_main_only_protection(self):
        import os
        context = {"GITHUB_REPOSITORY": "jwmcglynn/donner", "GITHUB_REF": "refs/heads/main",
                   "GITHUB_EVENT_NAME": "workflow_run", "GITHUB_SHA": "a" * 40,
                   "GITHUB_WORKFLOW_REF": "jwmcglynn/donner/.github/workflows/deploy_docs.yaml@refs/heads/main"}
        rules = [{"type": "deletion"}, {"type": "non_fast_forward"}]
        env = {"can_admins_bypass": False, "protection_rules": [],
               "deployment_branch_policy": {"protected_branches": False, "custom_branch_policies": True}}
        branches = {"total_count": 1, "branch_policies": [{"name": "main", "type": "branch"}]}
        api = store.GitHub()
        with mock.patch.dict(os.environ, context), mock.patch.object(api, "call", side_effect=[rules, env, branches]) as call:
            api.require_protection()
            self.assertIn("environments/github-pages-main", call.call_args_list[1].args[0])
        with mock.patch.dict(os.environ, context), mock.patch.object(api, "call", side_effect=[rules, {**env, "can_admins_bypass": True}, branches]):
            api.require_protection()
        with mock.patch.dict(os.environ, context), \
             mock.patch.object(api, "call", side_effect=[rules, {**env, "can_admins_bypass": None}]), \
             self.assertRaisesRegex(ValueError, "explicit administrator bypass"):
            api.require_protection()

    def test_unconfigured_snapshot_rules_block_the_writer(self):
        api = store.GitHub()
        with mock.patch.object(api, "call", return_value=[]) as call, \
             self.assertRaisesRegex(ValueError, "deletion and non-fast-forward"):
            api.require_protection()
        self.assertEqual(call.call_count, 1)

    def test_noncanonical_index_and_composition_ids_are_rejected(self):
        api = mock.Mock()
        api.tip.return_value = "a" * 40
        api.file.return_value = site.canonical({"schema": 1, "releases": {}, "current": "../x", "previous": None})
        with self.assertRaisesRegex(ValueError, "composition ID"):
            store.index(api, "a" * 40)


if __name__ == "__main__":
    unittest.main()
