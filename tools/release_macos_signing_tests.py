"""Fail-closed contracts for the signing-only derivative of an existing release."""

import base64
import copy
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile

from tools import release_macos_signing as signing


class SigningTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.team = "ABCDEFGHIJ"
        self.digest = "a" * 64
        self.assets = [
            {"name": signing.ARCHIVE, "id": 1, "size": 100, "digest": "sha256:" + self.digest},
            {"name": signing.ARCHIVE + ".sha256", "id": 2, "size": 97,
             "digest": "sha256:" + "b" * 64},
        ]
        self.release = {"tag_name": "v0.8.0", "draft": False, "immutable": False,
                        "id": 10, "assets": self.assets}
        self.signature = ("TeamIdentifier=" + self.team + "\n"
                          "Authority=Developer ID Application: Example (" + self.team + ")\n"
                          "Authority=Developer ID Certification Authority\n"
                          "Authority=Apple Root CA\n"
                          "CodeDirectory v=20500 size=99 flags=0x10000(runtime) hashes=1+7\n"
                          "Timestamp=Oct 25, 2026 at 12:00:00\n")

    def test_inputs_reject_ref_injection_and_abbreviated_identities(self):
        signing.check_inputs("v0.8.0", "a" * 40, self.digest)
        for tag in ("main", "v0.8.0/../../main", "v0.8.0;echo", "v0.8.0\n", "v0.8.0-rc1"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                signing.check_inputs(tag, "a" * 40, self.digest)
        for commit, digest in (("a" * 7, self.digest), ("a" * 40, "b" * 63)):
            with self.subTest(commit=commit), self.assertRaises(ValueError):
                signing.check_inputs("v0.8.0", commit, digest)

    def test_release_asset_selection_rejects_changed_missing_duplicate_or_oversized_assets(self):
        self.assertEqual(set(signing.release_assets(self.release, "v0.8.0", self.digest)),
                         {signing.ARCHIVE, signing.ARCHIVE + ".sha256"})
        changes = [{"tag_name": "v0.9.0"}, {"draft": True}, {"immutable": True},
                   {"assets": self.assets[:1]}, {"assets": self.assets + self.assets}]
        for change in changes:
            with self.subTest(change=change), self.assertRaises(ValueError):
                signing.release_assets({**self.release, **change}, "v0.8.0", self.digest)
        for field, value in (("id", True), ("id", -1), ("size", 0),
                             ("size", signing.native.MAX_ARCHIVE_BYTES + 1),
                             ("digest", "sha256:" + "c" * 64)):
            release = copy.deepcopy(self.release)
            release["assets"][0][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                signing.release_assets(release, "v0.8.0", self.digest)

    def test_wrong_team_adhoc_missing_timestamp_and_missing_runtime_are_rejected(self):
        signing.check_signature(self.signature, self.team)
        for fragment in ("TeamIdentifier=" + self.team, "Timestamp=", "runtime",
                         "Authority=Developer ID Certification Authority", "Authority=Apple Root CA"):
            with self.subTest(fragment=fragment), self.assertRaises(ValueError):
                signing.check_signature(self.signature.replace(fragment, "absent"), self.team)
        with self.assertRaises(ValueError):
            signing.check_signature(self.signature + "Signature=adhoc\n", self.team)

    def test_identity_requires_one_valid_developer_id_for_exact_team(self):
        fingerprint = "A" * 40
        line = f'  1) {fingerprint} "Developer ID Application: Example ({self.team})"\n'
        self.assertEqual(signing.signing_identity(line, self.team), fingerprint)
        for text in ("", line + line, line.replace(self.team, "OTHERTEAM1"),
                     line.replace("Developer ID Application", "Apple Development")):
            with self.subTest(text=text), self.assertRaises(ValueError):
                signing.signing_identity(text, self.team)

    def test_resources_allow_only_signatures_images_and_stapled_ticket_to_change(self):
        prefix = signing.native.BUNDLE + "/Contents/"
        original = {name: {"sha256": "old", "mode": 0o644} for name in (
            "LICENSE", "NOTICE.txt", prefix + "Info.plist", prefix + "MacOS/Donner",
            prefix + "Frameworks/libExample.dylib", prefix + "_CodeSignature/CodeResources")}
        final = copy.deepcopy(original)
        final[prefix + "MacOS/Donner"]["sha256"] = "signed"
        final[prefix + "Frameworks/libExample.dylib"]["sha256"] = "signed"
        final[prefix + "_CodeSignature/CodeResources"]["sha256"] = "signed"
        final[prefix + "CodeResources"] = {"sha256": "ticket", "mode": 0o644}
        signing.check_resources(original, final)
        for name in ("LICENSE", "NOTICE.txt", prefix + "Info.plist"):
            bad = copy.deepcopy(final)
            bad[name]["sha256"] = "modified"
            with self.subTest(name=name), self.assertRaises(ValueError):
                signing.check_resources(original, bad)
        with self.assertRaises(ValueError):
            signing.check_resources(original, {**final, prefix + "MacOS/extra": {}})
        with self.assertRaises(ValueError):
            signing.check_resources(original, {k: v for k, v in final.items() if k != "LICENSE"})

    def test_archive_traversal_is_rejected_before_extraction(self):
        source = self.root / "bad.zip"
        with zipfile.ZipFile(source, "w") as archive:
            entry = zipfile.ZipInfo("../outside")
            entry.external_attr = (stat.S_IFREG | 0o644) << 16
            archive.writestr(entry, "bad")
        with self.assertRaises(ValueError):
            signing.extract(source, self.root / "expanded")
        self.assertFalse((self.root / "expanded").exists())
        self.assertFalse((self.root / "outside").exists())

    def test_failed_platform_command_does_not_disclose_credentials(self):
        secret = "fixture-private-password"
        error = subprocess.CalledProcessError(1, ["security", "-p", secret], stderr=secret.encode())
        with mock.patch.object(signing.subprocess, "run", side_effect=error):
            with self.assertRaises(ValueError) as caught:
                signing.run("/usr/bin/security", "-p", secret)
        self.assertNotIn(secret, str(caught.exception))
        self.assertIsNone(caught.exception.__cause__)

    def test_keychain_failure_retains_safe_actionable_diagnostic(self):
        import contextlib
        import io
        output = io.StringIO()
        path = self.root / "signing-errors.json"
        secret = "fixture-private-password"
        failure = subprocess.CalledProcessError(1, ["security", "import", "-P", secret],
                                                output=b"untrusted certificate", stderr=(
                                                    "MAC verification failed: " + secret).encode())
        with mock.patch.object(signing, "DIAGNOSTIC_PATH", path, create=True), \
             mock.patch.object(signing.subprocess, "run", side_effect=failure), \
             contextlib.redirect_stderr(output), self.assertRaises(ValueError):
            signing.run("/usr/bin/security", "import", "fixture.p12", "-P", secret)
        diagnostic = json.loads(path.read_text())[0]
        self.assertEqual(diagnostic["operation"], "import")
        self.assertEqual(diagnostic["reason"], "pkcs12-password-or-data")
        self.assertEqual(diagnostic["exit_code"], 1)
        self.assertNotIn(secret, path.read_text() + output.getvalue())
        self.assertNotIn("fixture.p12", path.read_text() + output.getvalue())
        self.assertIn("pkcs12-password-or-data", output.getvalue())

    def test_timeout_retains_tool_and_bounded_classification(self):
        import contextlib
        import io
        path = self.root / "signing-errors.json"
        failure = subprocess.TimeoutExpired(["codesign", "secret"], 120,
                                            stderr=b"secret" * 20000)
        with mock.patch.object(signing, "DIAGNOSTIC_PATH", path, create=True), \
             mock.patch.object(signing.subprocess, "run", side_effect=failure), \
             contextlib.redirect_stderr(io.StringIO()), self.assertRaises(ValueError):
            signing.run("/usr/bin/codesign", "--sign", "secret", "app")
        record = json.loads(path.read_text())[0]
        self.assertEqual(record["tool"], "codesign")
        self.assertEqual(record["reason"], "timeout")
        self.assertTrue(record["output_truncated"])
        self.assertLess(path.stat().st_size, 1024)
        self.assertNotIn("secret", path.read_text())

    def test_keychain_and_certificate_cleanup_runs_on_signing_failure(self):
        values = {name: "fixture" for name in signing.SECRET_NAMES}
        values["MACOS_CERTIFICATE_BASE64"] = base64.b64encode(b"fixture-p12").decode() + "\n"
        calls = []
        def platform(*args, **kwargs):
            calls.append(args)
            if "create-keychain" in args:
                Path(args[-1]).write_text("fixture-keychain")
            if "delete-keychain" in args:
                Path(args[-1]).unlink()
            return ""
        with mock.patch.dict(os.environ, values), mock.patch.object(signing, "run", side_effect=platform):
            with self.assertRaisesRegex(ValueError, "test failure"):
                with signing.signing_keychain(self.root):
                    self.assertTrue((self.root / "private/identity.p12").is_file())
                    raise ValueError("test failure")
            for name in signing.SECRET_NAMES:
                self.assertNotIn(name, os.environ)
        self.assertFalse((self.root / "private/identity.p12").exists())
        self.assertFalse((self.root / "private/signing.keychain-db").exists())
        self.assertTrue(any("delete-keychain" in args for args in calls))

    def test_rejected_notarization_preserves_request_and_log(self):
        request_id = "11111111-1111-1111-1111-111111111111"
        result = subprocess.CompletedProcess([], 1, stdout=b'{"status":"Invalid"}', stderr=b"")
        with mock.patch.object(signing.subprocess, "run", return_value=result), \
             mock.patch.object(signing, "run", return_value='{"status":"Invalid","issues":[]}'):
            with self.assertRaisesRegex(ValueError, "did not accept"):
                signing.wait_notarization(self.root, self.root / "keychain", request_id)
        self.assertTrue((self.root / "notary-log.json").exists())
        status = json.loads((self.root / "notary-status.json").read_text())
        self.assertEqual(status, {"id": request_id, "status": "Invalid"})

    def test_wait_timeout_preserves_request_without_claiming_acceptance(self):
        request_id = "11111111-1111-1111-1111-111111111111"
        error = subprocess.TimeoutExpired(["notarytool"], 1260)
        with mock.patch.object(signing.subprocess, "run", side_effect=error), \
             mock.patch.object(signing, "run") as platform:
            with self.assertRaisesRegex(ValueError, "retained request ID"):
                signing.wait_notarization(self.root, self.root / "keychain", request_id)
            platform.assert_not_called()
        self.assertEqual(json.loads((self.root / "notary-status.json").read_text()),
                         {"id": request_id, "status": "Unknown"})

    def test_backfill_rejects_recreated_assets_other_releases_and_record_changes(self):
        policy = signing.BACKFILL
        release = {**self.release, "id": policy["release_id"], "body": policy["candidate_record"]}
        assets = {a["name"]: dict(a) for a in self.assets}
        assets[signing.ARCHIVE]["id"] = policy["archive_id"]
        assets[signing.ARCHIVE + ".sha256"]["id"] = policy["checksum_id"]
        assets[signing.ARCHIVE + ".sha256"]["digest"] = "sha256:" + policy["checksum_sha256"]
        args = (policy["tag"], policy["commit"], policy["archive_sha256"])
        signing.check_backfill(release, assets, *args)
        for change in ({"id": policy["release_id"] + 1}, {"body": ""},
                       {"body": policy["candidate_record"] + "\n" + policy["candidate_record"]}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                signing.check_backfill({**release, **change}, assets, *args)
        changed = copy.deepcopy(assets)
        changed[signing.ARCHIVE]["id"] += 1
        with self.assertRaises(ValueError):
            signing.check_backfill(release, changed, *args)
        with self.assertRaises(ValueError):
            signing.check_backfill(release, assets, "v0.9.0", args[1], args[2])

    def test_attestation_rejects_different_bytes_than_the_verifier_checked(self):
        (self.root / signing.ARCHIVE).write_bytes(b"signed package")
        (self.root / "receipt.json").write_text('{"receipt":"fixture"}')
        checksum = self.root / (signing.ARCHIVE + ".sha256")
        checksum.write_text("fixture checksum")
        proof = {"archive_sha256": signing.native.release_cli.digest(self.root / signing.ARCHIVE),
                 "receipt_sha256": signing.native.release_cli.digest(self.root / "receipt.json"),
                 "checksum_sha256": signing.native.release_cli.digest(checksum), "entrypoint": True}
        (self.root / "verification.json").write_text(json.dumps(proof))
        signing.check_verification(self.root)
        checksum.write_text("mutated sidecar")
        with self.assertRaises(ValueError):
            signing.check_verification(self.root)
        checksum.write_text("fixture checksum")
        (self.root / signing.ARCHIVE).write_bytes(b"changed package")
        with self.assertRaises(ValueError):
            signing.check_verification(self.root)

    def test_notary_log_upload_is_bounded_and_redacts_credentials(self):
        raw = json.dumps({"ignored": "private field", "issues": [
            {"severity": "error", "path": "private path", "message": "fixture-password failed"}]})
        with mock.patch.dict(os.environ, {"APPLE_APP_SPECIFIC_PASSWORD": "fixture-password"}):
            summary = signing.notary_log_summary(raw)
        self.assertEqual(summary, {"issues": [{"severity": "error", "message": "[redacted] failed"}]})
        with self.assertRaises(ValueError):
            signing.notary_log_summary(json.dumps({"issues": [{}] * 101}))

    def test_loader_probe_receives_only_explicit_environment(self):
        def execute(*args, **kwargs):
            kwargs["stderr"].write(b"Unknown option --donner-release-loader-check\nUsage: donner-editor ")
            return subprocess.CompletedProcess([], 1)
        minimal = {"PATH": "/usr/bin:/bin", "HOME": str(self.root)}
        with mock.patch.dict(os.environ, {"ACTIONS_ID_TOKEN_REQUEST_TOKEN": "fixture-secret"}), \
             mock.patch.object(signing.native.subprocess, "run", side_effect=execute) as execute_mock:
            signing.native._check_entrypoint(self.root / "app", environment=minimal)
        self.assertEqual(execute_mock.call_args.kwargs["env"], minimal)

    def test_gatekeeper_acceptance_requires_notarized_developer_id(self):
        def platform(*args, **kwargs):
            if "--display" in args:
                return self.signature
            if "--assess" in args:
                return "accepted\nsource=Developer ID\n"
            return ""
        with mock.patch.object(signing, "run", side_effect=platform):
            with self.assertRaisesRegex(ValueError, "Gatekeeper"):
                signing.verify_signed(self.root / "app", self.team, notarized=True)


if __name__ == "__main__":
    unittest.main()
