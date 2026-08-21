"""Tests for immutable, resumable release asset publication."""

from pathlib import Path
import json
import subprocess
import tempfile
import unittest
from unittest import mock

from tools import release_artifact_publisher as publisher


class ResolveRemoteTagTest(unittest.TestCase):
    def test_lightweight_tag(self):
        self.assertEqual(
            publisher.resolve_remote_tag("a" * 40 + "\trefs/tags/v1\n", "v1"),
            "a" * 40,
        )

    def test_annotated_tag_prefers_peeled_commit_in_any_order(self):
        refs = "\n".join(
            [
                "b" * 40 + "\trefs/tags/v1^{}",
                "a" * 40 + "\trefs/tags/v1",
            ]
        )
        self.assertEqual(publisher.resolve_remote_tag(refs, "v1"), "b" * 40)
        self.assertIsNone(publisher.resolve_remote_tag(refs, "v2"))

    @mock.patch("tools.release_artifact_publisher.subprocess.run")
    def test_remote_verification_accepts_only_expected_peeled_commit(self, run):
        run.return_value = subprocess.CompletedProcess(
            [],
            0,
            stdout="a" * 40 + "\trefs/tags/v1\n" + "b" * 40 + "\trefs/tags/v1^{}\n",
            stderr="",
        )
        publisher.verify_remote_tag("v1", "b" * 40)
        with self.assertRaisesRegex(RuntimeError, "does not resolve"):
            publisher.verify_remote_tag("v1", "a" * 40)


class PublishAssetsTest(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.path = Path(self.temp_dir.name) / "artifact.bin"
        self.path.write_bytes(b"verified bytes")
        self.publisher = publisher.Publisher("owner/repo", "7", "https://uploads.test/assets{?name}")

    def tearDown(self):
        self.temp_dir.cleanup()

    def completed(self, returncode=0, stdout=""):
        return subprocess.CompletedProcess([], returncode, stdout=stdout, stderr="")

    def assets(self, *values):
        return json.dumps(list(values))

    @mock.patch("tools.release_artifact_publisher.subprocess.run")
    def test_matching_asset_is_not_uploaded_again(self, run):
        digest = publisher.sha256_digest(self.path)
        run.return_value = self.completed(
            stdout=self.assets({"name": self.path.name, "digest": digest})
        )

        self.publisher.publish([self.path])

        self.assertEqual(run.call_count, 1)
        self.assertNotIn("POST", run.call_args.args[0])

    @mock.patch("tools.release_artifact_publisher.subprocess.run")
    def test_mismatched_asset_fails_closed(self, run):
        run.return_value = self.completed(
            stdout=self.assets({"name": self.path.name, "digest": "sha256:bad"})
        )

        with self.assertRaisesRegex(RuntimeError, "different digest"):
            self.publisher.publish([self.path])

        self.assertEqual(run.call_count, 1)

    @mock.patch("tools.release_artifact_publisher.subprocess.run")
    def test_absent_asset_uploads_once(self, run):
        run.side_effect = [self.completed(stdout="[]"), self.completed()]

        self.publisher.publish([self.path])

        self.assertEqual(run.call_count, 2)
        self.assertIn("POST", run.call_args.args[0])

    @mock.patch("tools.release_artifact_publisher.subprocess.run")
    def test_lost_success_response_is_recovered_from_published_digest(self, run):
        digest = publisher.sha256_digest(self.path)
        run.side_effect = [
            self.completed(stdout="[]"),
            self.completed(returncode=1),
            self.completed(stdout=self.assets({"name": self.path.name, "digest": digest})),
        ]

        self.publisher.publish([self.path])

        self.assertEqual(run.call_count, 3)

    @mock.patch("tools.release_artifact_publisher.subprocess.run")
    def test_stops_after_three_failed_uploads(self, run):
        run.side_effect = [
            self.completed(stdout="[]"),
            self.completed(returncode=1),
            self.completed(stdout="[]"),
            self.completed(returncode=1),
            self.completed(stdout="[]"),
            self.completed(returncode=1),
            self.completed(stdout="[]"),
        ]

        with self.assertRaisesRegex(RuntimeError, "after 3 attempts"):
            self.publisher.publish([self.path])

        post_calls = [call for call in run.call_args_list if "POST" in call.args[0]]
        self.assertEqual(len(post_calls), 3)


if __name__ == "__main__":
    unittest.main()
