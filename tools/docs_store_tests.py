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
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.archive = self.root / "site.tar"; self.archive.write_bytes(b"independent main and release bytes" * 500)
        self.payload = self.root / "payload"; self.payload.mkdir()

    def test_roundtrip_survives_actions_expiry_without_github_artifacts(self):
        record = store.split_archive(self.archive, self.payload)
        output = self.root / "restored.tar"
        store.restore(record, output, lambda name: (self.payload / name).read_bytes())
        self.assertEqual(output.read_bytes(), self.archive.read_bytes())

    def test_corrupted_missing_and_oversized_chunks_are_rejected(self):
        record = store.split_archive(self.archive, self.payload)
        for failure in (b"changed", b"", b"x" * (store.CHUNK + 1)):
            with self.subTest(size=len(failure)), self.assertRaisesRegex(ValueError, "chunk changed"):
                store.restore(record, self.root / "bad.tar", lambda name: failure)

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

    def test_pinned_branch_advance_blocks_before_any_blob_write(self):
        api = mock.Mock(); api.tip.return_value = "b" * 40
        with self.assertRaisesRegex(ValueError, "advanced"):
            store.commit(api, "a" * 40, self.payload, {})
        api.call.assert_not_called()

    def test_existing_immutable_entry_cannot_be_replaced(self):
        (self.payload / "objects").mkdir(); (self.payload / "objects/x").write_bytes(b"new")
        api = mock.Mock(); api.tip.return_value = "a" * 40; api.file.return_value = b"old"
        with self.assertRaisesRegex(ValueError, "immutable storage entry"):
            store.commit(api, "a" * 40, self.payload, {})
        api.call.assert_not_called()

    def test_failure_reading_old_entry_does_not_create_or_overwrite_it(self):
        (self.payload / "objects").mkdir(); (self.payload / "objects/x").write_bytes(b"new")
        api = mock.Mock(); api.tip.return_value = "a" * 40; api.file.side_effect = ValueError("permission denied")
        with self.assertRaisesRegex(ValueError, "cannot inspect"):
            store.commit(api, "a" * 40, self.payload, {})
        api.call.assert_not_called()

    def test_actual_subprocess_timeout_becomes_an_unknown_outcome(self):
        import subprocess
        with mock.patch.object(store.subprocess, "run", side_effect=subprocess.TimeoutExpired("gh", 180)), \
             self.assertRaisesRegex(ValueError, "outcome is unknown"):
            store.GitHub().call("repos/jwmcglynn/donner/pages/deployments", "POST", {"artifact_id": 1})

    def test_noncanonical_index_and_composition_ids_are_rejected(self):
        api = mock.Mock(); api.tip.return_value = "a" * 40
        api.file.return_value = site.canonical({"schema": 1, "releases": {}, "current": "../x", "previous": None})
        with self.assertRaisesRegex(ValueError, "composition ID"):
            store.index(api, "a" * 40)


if __name__ == "__main__": unittest.main()
