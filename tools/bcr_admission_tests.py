"""Tests for the local archive transport used with upstream BCR validation."""

from enum import Enum
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from tools import bcr_admission as admission


class Result(Enum):
    GOOD = 1
    NEED_BCR_MAINTAINER_REVIEW = 2
    FAILED = 3


class AdmissionBridgeTest(unittest.TestCase):
    @mock.patch.object(admission.subprocess, "check_output", return_value="d" * 40)
    def test_upstream_failures_and_review_requirements_remain_distinct(self, git):
        for upstream_status, expected_status in [(0, 0), (42, 0), (1, 1)]:
            with self.subTest(status=upstream_status), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                archive = root / "donner.tar.gz"
                archive.write_bytes(b"candidate bytes")
                original = mock.Mock(return_value=b"live registry data")
                original_file = mock.Mock()
                validation = SimpleNamespace(download=original, download_file=original_file,
                                             UPSTREAM_MODULES_DIR_URL="https://bcr.bazel.build/modules")
                test = self

                class Validator:
                    def __init__(self, registry, upstream, should_fix):
                        test.assertFalse(should_fix)
                        result = {0: Result.GOOD, 42: Result.NEED_BCR_MAINTAINER_REVIEW, 1: Result.FAILED}[upstream_status]
                        self.validation_results = [(result, "upstream diagnostic")]

                    def validate_module(self, name, version, skips):
                        test.assertEqual((name, version, skips), ("donner", "1.0.0", []))
                        test.assertEqual(validation.download("https://release.test/archive"), b"candidate bytes")
                        destination = root / "downloaded"
                        validation.download_file("https://release.test/archive", destination)
                        test.assertEqual(destination.read_bytes(), b"candidate bytes")
                        test.assertEqual(validation.download("https://registry.test/metadata"), b"live registry data")

                    def validate_metadata(self, names):
                        test.assertEqual(names, ["donner"])

                    def global_checks(self):
                        pass

                    def getValidationReturnCode(self):
                        return upstream_status

                validation.BcrValidator = Validator
                api = SimpleNamespace(RegistryClient=lambda path: path,
                                      UpstreamRegistry=lambda **kwargs: kwargs)
                report = root / "report.json"
                status = admission.run_validator(validation, api, root, root, archive, "1.0.0",
                                                  "https://release.test/archive", report)
                self.assertEqual(status, expected_status)
                self.assertIs(validation.download, original)
                self.assertIs(validation.download_file, original_file)
                original.assert_called_once_with("https://registry.test/metadata")
                result = json.loads(report.read_text())
                self.assertEqual(result["upstream_exit_code"], upstream_status)
                self.assertEqual(result["validator_revision"], "d" * 40)

    def test_transport_is_restored_after_an_upstream_exception(self):
        original = mock.Mock()
        original_file = mock.Mock()
        validation = SimpleNamespace(download=original, download_file=original_file,
                                     UPSTREAM_MODULES_DIR_URL="https://bcr.bazel.build/modules",
                                     BcrValidator=mock.Mock(side_effect=RuntimeError("upstream failure")))
        api = SimpleNamespace(RegistryClient=lambda path: path,
                              UpstreamRegistry=lambda **kwargs: kwargs)
        with self.assertRaisesRegex(RuntimeError, "upstream failure"):
            admission.run_validator(validation, api, Path("."), Path("."), Path("archive"), "1.0.0",
                                    "https://release.test/archive", Path("report"))
        self.assertIs(validation.download, original)
        self.assertIs(validation.download_file, original_file)


if __name__ == "__main__":
    unittest.main()
