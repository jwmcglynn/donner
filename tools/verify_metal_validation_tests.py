"""Reject stale, incomplete, skipped, or relaxed Metal validation evidence."""

import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import metal_validation_profile as profile
import verify_metal_validation as verifier


FULL = "METAL_VALIDATION_PROFILE profile=full validation_state=enabled"
NARROW = "METAL_VALIDATION_PROFILE profile=paravirtual-texture-usage-off validation_state=disabled"
PASSED_XML = b'<testsuites><testsuite tests="1"><testcase name="renders" status="run"/></testsuite></testsuites>'


class MetalEvidenceTest(unittest.TestCase):
    def test_profile_requires_one_consistent_marker(self):
        self.assertEqual("full", profile.parse_profile("test wrapper\n" + FULL + "\n"))
        self.assertEqual("paravirtual-texture-usage-off", profile.parse_profile(NARROW))
        for text in ("", FULL + "\n" + FULL, FULL + "\n" + NARROW,
                     FULL.replace("enabled", "disabled"), FULL + " trailing", " " + FULL):
            with self.subTest(text=text), self.assertRaises(ValueError):
                profile.parse_profile(text)

    def test_xml_requires_executed_cases_without_skips_or_failures(self):
        self.assertEqual(1, verifier.validate_xml(PASSED_XML))
        for data in (b'<testsuites/>', b'<testsuite><testcase><skipped/></testcase></testsuite>',
                     b'<testsuite failures="1"><testcase/></testsuite>',
                     b'<testsuite disabled="1"><testcase/></testsuite>',
                     b'<testsuite><testcase status="notrun"/></testsuite>',
                     b'<testsuite><testcase><error/></testcase></testsuite>'):
            with self.subTest(data=data), self.assertRaises(ValueError):
                verifier.validate_xml(data)

    def test_remote_output_must_match_digest_and_size(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test.log"
            path.write_bytes(b"current output")
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            uri = f"bytestream://example.invalid/blobs/{digest}/{path.stat().st_size}"
            self.assertEqual(b"current output", verifier.output_digest(uri, path)[0])
            path.write_bytes(b"stale output")
            with self.assertRaises(ValueError):
                verifier.output_digest(uri, path)
            with self.assertRaises(ValueError):
                verifier.output_digest("https://example.invalid/test.log", path)

    def test_dirty_source_cannot_qualify(self):
        with mock.patch.object(verifier, "git", return_value=" M changed.cc"):
            with self.assertRaises(ValueError):
                verifier.clean_revision(Path("."))

    def collect_fixture(self, marker=FULL, status="PASSED", cached=False, include_summary=True,
                        shard_count=1, result_shards=None, cached_remote=False,
                        summary_configuration="test-config", finished=True):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            logs = root / "testlogs"
            directory = logs / "donner/gpu/metal/tests/metal_full_validation_required"
            label = verifier.CAPABILITY
            events = [{"started": {"uuid": "test-invocation", "buildToolVersion": "8"}}]
            if result_shards is None:
                result_shards = list(range(1, shard_count + 1))
            for shard in result_shards:
                shard_directory = directory
                if shard_count > 1:
                    shard_directory /= f"shard_{shard}_of_{shard_count}"
                shard_directory.mkdir(parents=True, exist_ok=True)
                (shard_directory / "test.log").write_text(marker + "\n")
                (shard_directory / "test.xml").write_bytes(PASSED_XML)
                events.append({"id": {"testResult": {"label": label, "shard": shard,
                                                      "configuration": {"id": "test-config"}}},
                               "testResult": {"status": status, "cachedLocally": cached,
                                              "executionInfo": {"cachedRemotely": cached_remote},
                                              "testActionOutput": [
                                                  {"name": name, "uri": (shard_directory / name).as_uri()}
                                                  for name in ("test.log", "test.xml")]}})
            if include_summary:
                events.append({"id": {"testSummary": {"label": label,
                                                       "configuration": {"id": summary_configuration}}},
                               "testSummary": {"overallStatus": "PASSED", "shardCount": shard_count,
                                               "totalRunCount": shard_count}})
            if finished:
                events.append({"finished": {"exitCode": {"name": "SUCCESS"}}})
            bep = root / "events.json"
            bep.write_text("\n".join(json.dumps(event) for event in events))
            output = root / "evidence"
            output.mkdir()
            return verifier.collect_results(bep, logs, (label,), output)

    def test_every_uncached_shard_and_matching_configuration_is_required(self):
        self.assertEqual(2, len(self.collect_fixture(shard_count=2)["tests"]))
        for kwargs in ({"shard_count": 2, "result_shards": [1]},
                       {"result_shards": [1, 1]}, {"result_shards": [2]},
                       {"cached_remote": True}, {"finished": False},
                       {"summary_configuration": "different"}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                self.collect_fixture(**kwargs)

    def test_capability_receipt_accepts_only_full_fresh_complete_evidence(self):
        self.assertEqual(1, self.collect_fixture()["tests"][0]["cases"])
        for kwargs in ({"marker": NARROW}, {"marker": ""}, {"status": "FAILED"},
                       {"cached": True}, {"include_summary": False}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                self.collect_fixture(**kwargs)


if __name__ == "__main__":
    unittest.main()
