"""Publication authority, exact-input binding and truthful failure/recovery regressions."""
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from tools import docs_publish as publish
from tools import docs_site as site


class DocsPublishTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.record = {"manifest_sha256": "a" * 64, "workflow_commit": "b" * 40}
        self.prior = {"kind": "composition", "manifest_sha256": "c" * 64}
        self.api = mock.Mock()
        self.api.tip.return_value = "e" * 40
        self.api.call.return_value = {"status": "succeed"}

    def test_request_cannot_override_workflow_context_or_smuggle_extra_fields(self):
        request = {"composition_run": "123/1", "composition_sha256": "a" * 64, "prior_composition": "b" * 64}
        with mock.patch.dict(os.environ, {"DOCS_REQUEST": json.dumps(request)}):
            self.assertEqual(publish.workflow_request("publish"), request)
        for key, value in (("GH_TOKEN", "secret"), ("source_commit", "c" * 40)):
            with mock.patch.dict(os.environ, {"DOCS_REQUEST": json.dumps({**request, key: value})}), self.assertRaisesRegex(ValueError, "unexpected"):
                publish.workflow_request("publish")

    def test_context_requires_exact_repository_manual_main(self):
        good = {"GITHUB_REPOSITORY": "jwmcglynn/donner", "GITHUB_REF": "refs/heads/main", "GITHUB_EVENT_NAME": "workflow_dispatch"}
        with mock.patch.dict(os.environ, good):
            publish.workflow_context()
        for key, value in (("GITHUB_REF", "refs/heads/feature"), ("GITHUB_EVENT_NAME", "push"), ("GITHUB_REPOSITORY", "fork/donner")):
            with mock.patch.dict(os.environ, {**good, key: value}), self.assertRaisesRegex(ValueError, "trusted-main"):
                publish.workflow_context()

    def test_legacy_seed_needs_literal_approval_and_uninitialized_store(self):
        request = {"legacy_run": "1/1", "legacy_artifact_id": "2", "legacy_zip_sha256": "a" * 64,
                   "legacy_archive_sha256": "b" * 64, "approve_legacy_rollback_seed": False}
        with mock.patch.dict(os.environ, {"DOCS_REQUEST": json.dumps(request)}), self.assertRaisesRegex(ValueError, "invalid docs request"):
            publish.workflow_request("seed")
        api = mock.Mock()
        api.tip.return_value = "a" * 40
        with mock.patch.dict(os.environ, {"APPROVE_LEGACY_ROLLBACK_SEED": "true"}), self.assertRaisesRegex(ValueError, "before the snapshot"):
            publish.seed_legacy("1/1", 2, "a" * 64, "b" * 64, self.root / "seed", api)
        api.call.assert_not_called()

    def test_preflight_branch_advance_and_prior_mismatch_block_before_publication(self):
        record = {**self.record, "base_snapshot_commit": "a" * 40, "previous_composition": "d" * 64}
        with mock.patch.object(publish, "fetch_composition", return_value=record), \
             mock.patch.object(publish.store, "index", return_value={"current": "e" * 64}), \
             mock.patch.object(publish, "deploy") as deploy, self.assertRaisesRegex(ValueError, "prior composition"):
            publish.preflight("1/1", "f" * 64, "d" * 64, self.root / "preflight", mock.Mock())
        deploy.assert_not_called()

    def test_preflight_requires_a_durable_previous_site_before_writes(self):
        record = {**self.record, "base_snapshot_commit": None, "previous_composition": None}
        with mock.patch.object(publish, "fetch_composition", return_value=record), \
             mock.patch.object(publish.store, "index", return_value={"current": None}), \
             self.assertRaisesRegex(ValueError, "bootstrap"):
            publish.preflight("1/1", "f" * 64, None, self.root / "preflight", mock.Mock())

    def test_recovery_reports_unchanged_when_old_site_is_still_live(self):
        self.api.call.return_value = {"status": "deployment_failed"}
        with mock.patch.object(publish, "_live_identity", return_value=True), mock.patch.object(publish, "deploy") as deploy:
            result = publish._recover(self.record, self.prior, 7, self.api, "POST lost response", "e" * 40, "91")
        self.assertEqual(result["status"], "unchanged")
        deploy.assert_not_called()

    def test_recovery_restores_only_known_candidate_with_verified_prior(self):
        with mock.patch.object(publish, "_live_identity", side_effect=[False, True]), \
             mock.patch.object(publish, "live_marker", return_value={"manifest_sha256": "a" * 64}), \
             mock.patch.object(publish, "deploy", return_value="deployment-9") as deploy:
            result = publish._recover(self.record, self.prior, 7, self.api, "health check failed", "e" * 40, "91")
        self.assertEqual(result["status"], "restored")
        self.assertEqual(deploy.call_args.args[0], 7)

    def test_unknown_live_identity_stops_without_a_second_pages_post(self):
        for live in (None, {"manifest_sha256": "d" * 64}):
            with mock.patch.object(publish, "_live_identity", return_value=False), \
                 mock.patch.object(publish, "live_marker", return_value=live), mock.patch.object(publish, "deploy") as deploy:
                result = publish._recover(self.record, self.prior, 7, self.api, "ambiguous", "e" * 40, "91")
            self.assertEqual(result["status"], "recovery-required")
            deploy.assert_not_called()

    def test_failed_restoration_cannot_be_reported_as_success(self):
        with mock.patch.object(publish, "_live_identity", return_value=False), \
             mock.patch.object(publish, "live_marker", return_value={"manifest_sha256": "a" * 64}), \
             mock.patch.object(publish, "deploy", side_effect=ValueError("restore rejected")):
            result = publish._recover(self.record, self.prior, 7, self.api, "candidate unhealthy", "e" * 40, "91")
        self.assertEqual(result["status"], "recovery-required")
        self.assertIn("restore rejected", result["recovery_error"])

    def test_network_failure_during_reconciliation_keeps_truthful_recovery_state(self):
        with mock.patch.object(publish, "_live_identity", side_effect=OSError("offline")):
            result = publish._recover(self.record, self.prior, 7, self.api, "POST timed out", "e" * 40, "91")
        self.assertEqual(result["status"], "recovery-required")

    def test_uploaded_recovery_artifact_cannot_come_from_another_run(self):
        meta = {"expired": False, "name": "docs-rollback-1", "workflow_run": {"id": 9, "head_sha": "a" * 40}}
        with mock.patch.dict(os.environ, {"GITHUB_RUN_ID": "8", "GITHUB_SHA": "a" * 40, "GITHUB_RUN_ATTEMPT": "1"}), \
             mock.patch.object(publish, "github_json", return_value=meta), \
             mock.patch.object(publish, "download") as download, self.assertRaisesRegex(ValueError, "not from this trusted run"):
            publish._verified_previous(7, {}, self.root)
        download.assert_not_called()

    def test_manual_composition_cannot_be_a_push_pr_or_failed_run(self):
        good = {"id": 123, "run_attempt": 1, "repository": {"full_name": "jwmcglynn/donner"},
                "path": publish.WORKFLOW, "event": "workflow_dispatch", "head_branch": "main",
                "head_sha": "a" * 40, "status": "completed", "conclusion": "success"}
        for change in ({"event": "push"}, {"event": "pull_request"}, {"head_branch": "feature"}, {"conclusion": "failure"}):
            with mock.patch.object(publish, "github_json", return_value={**good, **change}), \
                 self.assertRaisesRegex(ValueError, "successful trusted-main"):
                publish.fetch_composition("123/1", "b" * 64, self.root)

    def test_unrelated_branch_advance_blocks_automatic_restoration(self):
        self.api.tip.return_value = "f" * 40
        with mock.patch.object(publish, "deploy") as deploy:
            result = publish._recover(self.record, self.prior, 7, self.api, "activation failed", "e" * 40, "91")
        self.assertEqual(result["status"], "recovery-required")
        deploy.assert_not_called()
        self.api.call.assert_not_called()

    def test_lost_post_identity_and_inflight_request_do_not_race_a_rollback(self):
        for deployment, state in ((None, "succeed"), ("91", "building")):
            self.api.call.return_value = {"status": state}
            with mock.patch.object(publish, "_live_identity", return_value=False), \
                 mock.patch.object(publish, "live_marker", return_value={"manifest_sha256": "a" * 64}), \
                 mock.patch.object(publish, "deploy") as deploy:
                result = publish._recover(self.record, self.prior, 7, self.api, "POST failed", "e" * 40, deployment)
            self.assertEqual(result["status"], "recovery-required")
            deploy.assert_not_called()

    def test_inflight_timeout_is_not_unchanged_even_if_old_site_is_visible(self):
        self.api.call.return_value = {"status": "building"}
        with mock.patch.object(publish, "_live_identity", return_value=True), mock.patch.object(publish, "deploy") as deploy:
            result = publish._recover(self.record, self.prior, 7, self.api, "timeout", "e" * 40, "91")
        self.assertEqual(result["status"], "recovery-required")
        deploy.assert_not_called()

    def test_subprocess_timeout_cannot_escape_recovery_receipt(self):
        import subprocess
        self.api.call.side_effect = subprocess.TimeoutExpired("gh", 180)
        result = publish._recover(self.record, self.prior, 7, self.api, "timeout", "e" * 40, "91")
        self.assertEqual(result["status"], "recovery-required")

    def test_live_hash_inventory_cannot_be_replaced_alongside_changed_content(self):
        original = {"manifest_sha256": "a" * 64, "files": {"index.html": "b" * 64}}
        changed = {"manifest_sha256": "a" * 64, "files": {"index.html": "c" * 64}}
        with mock.patch.object(publish, "live_marker", return_value=changed), self.assertRaisesRegex(ValueError, "live composition differs"):
            publish._verify_live("a" * 64, original)

    def transaction(self):
        from tools import docs_fixture
        import shutil
        work = self.root / "transaction"
        work.mkdir()
        sources = {"0.8.0": docs_fixture.source(work, "release"), "main": docs_fixture.source(work, "main")}
        site.compose(sources, "0.8.0", work / "site")
        (work / "pages").mkdir()
        site.pack(work / "site", work / "pages/artifact.tar")
        payload = work / "payload"
        payload.mkdir()
        (payload / "selection.json").write_bytes(site.canonical({"releases": {}}))
        record = {"archive_sha256": publish.bcr_source.digest(work / "pages/artifact.tar"),
                  "manifest_sha256": site.read_json(work / "site/docs-site.json")["manifest_sha256"],
                  "workflow_commit": "b" * 40, "artifact_id": 7, "base_snapshot_commit": "a" * 40,
                  "payload_files": site.inventory(payload)}
        (payload / "composition.json").write_bytes(site.canonical(record))
        for name in ("composition-attestation.json", "pages-attestation.json"):
            (work / name).write_text('[]')
        plan = {"prior": "f" * 64, "prior_identity": self.prior,
                "current": {"schema": 1, "releases": {}, "current": "f" * 64, "previous": None}}
        self.api.tip.return_value = "e" * 40
        def commit(api, expected, folder, state):
            return "e" * 40
        def restore(record, output, read):
            shutil.copyfile(work / "pages/artifact.tar", output)
        return work, record, plan, commit, restore

    def test_actual_post_timeout_retains_a_recovery_required_receipt(self):
        import subprocess
        work, record, plan, commit, restore = self.transaction()
        with mock.patch.object(publish.store, "commit", side_effect=commit), \
             mock.patch.object(publish.store, "restore", side_effect=restore), \
             mock.patch.object(publish, "deploy", side_effect=subprocess.TimeoutExpired("gh", 180)), \
             self.assertRaisesRegex(ValueError, "inspect result.json"):
            publish._retain_and_deploy(record, plan, 9, work, self.api)
        self.assertEqual(site.read_json(work / "result.json")["status"], "recovery-required")

    def test_actual_activation_timeout_does_not_revert_healthy_content(self):
        import subprocess
        work, record, plan, commit, restore = self.transaction()
        with mock.patch.object(publish.store, "commit", side_effect=["e" * 40, subprocess.TimeoutExpired("gh", 180)]), \
             mock.patch.object(publish.store, "restore", side_effect=restore), \
             mock.patch.object(publish, "deploy", return_value="91") as deploy, \
             mock.patch.object(publish, "_verify_live"), self.assertRaisesRegex(ValueError, "inspect result.json"):
            publish._retain_and_deploy(record, plan, 9, work, self.api)
        self.assertEqual(site.read_json(work / "result.json")["status"], "recovery-required")
        self.assertEqual(deploy.call_count, 1)

    def test_all_pages_posts_require_live_protections_before_requesting_oidc(self):
        api = mock.Mock()
        api.require_protection.side_effect = ValueError("missing approval protection")
        with mock.patch.object(publish, "_oidc_token") as token, self.assertRaisesRegex(ValueError, "missing approval"):
            publish.deploy(7, "a" * 40, api)
        token.assert_not_called()
        api.call.assert_not_called()

    def test_self_consistent_forged_archive_and_index_fail_signed_source_binding(self):
        signed = {"source": {"version": "0.8.0", "commit": "a" * 40},
                  "artifacts": {"docs": {"run_id": 123, "attempt": 1,
                                          "files": {"artifact.tar": "b" * 64}}}}
        authentic = publish.release_identity(signed, "0.8.0")
        forged = {**authentic, "archive_sha256": "c" * 64}
        publish.require_release_binding(signed, "0.8.0", authentic, "b" * 64)
        with self.assertRaisesRegex(ValueError, "independently signed source"):
            publish.require_release_binding(signed, "0.8.0", forged, "c" * 64)

    def test_current_signed_roster_rejects_removed_or_replaced_release(self):
        identity = {"version": "0.8.0", "archive_sha256": "a" * 64}
        manifest = {"versions": {"0.8.0": {"identity": identity}, "main": {"identity": {}}}}
        for releases in ({}, {"0.8.0": {"identity": {**identity, "archive_sha256": "b" * 64}}}):
            with self.subTest(releases=releases), self.assertRaisesRegex(ValueError, "existing release"):
                publish.require_preserved_releases(manifest, releases)

    def test_prior_archive_cannot_be_replaced_under_an_approved_content_address(self):
        api = mock.Mock()
        api.file.return_value = site.canonical({"sha256": "b" * 64})
        with self.assertRaisesRegex(ValueError, "approved content address"):
            publish._previous(api, "a" * 40, "c" * 64, self.root)

    def test_forged_legacy_backup_cannot_change_the_independent_origin_zip(self):
        import zipfile
        import shutil
        authentic = self.root / "authentic.zip"
        with zipfile.ZipFile(authentic, "w") as archive:
            archive.writestr("artifact.tar", b"authentic legacy source")
        forged = self.root / "forged.tar"
        forged.write_bytes(b"forged assets with the same index")
        selected = {"artifact_id": 7, "run_id": 123, "attempt": 1,
                    "artifact_digest": "sha256:" + publish.bcr_source.digest(authentic),
                    "archive_sha256": publish.bcr_source.digest(forged)}
        legacy = {"selection": selected, "archive_sha256": selected["archive_sha256"]}
        with mock.patch.object(publish, "github_json", return_value={}), \
             mock.patch.object(publish.pages_artifact, "verify_metadata"), \
             mock.patch.object(publish, "download", side_effect=lambda number, path: shutil.copyfile(authentic, path)), \
             self.assertRaises(ValueError):
            publish._verify_legacy_origin(legacy, forged, self.root / "origin")

    def test_retained_release_proof_authenticates_the_release_candidate_workflow(self):
        record = {"source": {"version": "0.8.0", "commit": "a" * 40},
                  "artifacts": {"docs": {"run_id": 123, "attempt": 1,
                                          "files": {"artifact.tar": "b" * 64}}}}
        api = mock.Mock()
        api.file.return_value = b'{}'
        entry = {"identity": publish.release_identity(record, "0.8.0"), "archive": {"sha256": "b" * 64}}
        responses = [{"tag_name": "v0.8.0", "draft": False, "prerelease": False},
                     {"object": {"type": "commit", "sha": "a" * 40}}]
        with mock.patch.object(publish.candidate, "load_record", return_value=record), \
             mock.patch.object(publish, "_offline_attestation") as verify, \
             mock.patch.object(publish, "github_json", side_effect=responses):
            publish._verify_retained_release(api, "c" * 40, "0.8.0", entry, self.root)
        self.assertEqual(verify.call_args.kwargs["workflow"], ".github/workflows/release_candidate.yml")

    def test_health_wait_allows_public_propagation_without_entering_recovery(self):
        work, record, plan, commit, restore = self.transaction()
        with mock.patch.object(publish.store, "commit", side_effect=commit), \
             mock.patch.object(publish.store, "restore", side_effect=restore), \
             mock.patch.object(publish, "deploy", return_value="91"), \
             mock.patch.object(publish, "_verify_live", side_effect=[ValueError("old site"), None]), \
             mock.patch.object(publish.time, "sleep") as pause, \
             mock.patch.object(publish, "_recover") as recover:
            result = publish._retain_and_deploy(record, plan, 9, work, self.api)
        self.assertEqual(result["status"], "published")
        pause.assert_called_once_with(5)
        recover.assert_not_called()

    def test_health_wait_handles_temporary_network_errors(self):
        pause = mock.Mock()
        with mock.patch.object(publish, "_live_identity", side_effect=[OSError("offline"), False, True]) as check:
            publish._wait_for_live(self.prior, wait=pause)
        self.assertEqual(check.call_count, 3)
        self.assertEqual(pause.call_count, 2)

    def test_health_wait_stops_at_the_bounded_propagation_limit(self):
        pause = mock.Mock()
        with mock.patch.object(publish, "_live_identity", return_value=False) as check, \
             self.assertRaisesRegex(ValueError, "public site did not converge"):
            publish._wait_for_live(self.prior, wait=pause)
        self.assertEqual(check.call_count, 24)
        self.assertEqual(pause.call_count, 23)

    def test_network_requests_reject_other_schemes_and_embedded_credentials(self):
        for url in ("file:///secret", "http://example.test/", "https://user:secret@example.test/",
                    "https://example.test:444/", "https://example.test/#fragment"):
            with self.subTest(url=url), mock.patch.object(publish.urllib.request, "build_opener") as opener:
                with self.assertRaisesRegex(ValueError, "direct HTTPS"):
                    publish._https_response(url, {})
                opener.assert_not_called()

    def test_network_requests_refuse_redirects_and_keep_authorization_at_origin(self):
        with self.assertRaisesRegex(ValueError, "redirects"):
            publish._NoRedirects().redirect_request(None, None, 302, "Found", {}, "https://other.test/")
        opener = mock.Mock()
        with mock.patch.object(publish.urllib.request, "build_opener", return_value=opener) as build:
            publish._https_response("https://example.test/path?audience=pages", {"Authorization": "Bearer token"})
        self.assertIsInstance(build.call_args.args[0], publish._NoRedirects)
        self.assertEqual(opener.open.call_args.args[0].full_url, "https://example.test/path?audience=pages")
        self.assertEqual(opener.open.call_args.kwargs, {"timeout": 30})

    def test_pages_health_checks_detect_changed_version_index(self):
        marker = {"manifest_sha256": "a" * 64}
        with mock.patch.object(publish, "live_marker", return_value=marker), \
             mock.patch.object(publish, "_live_file", return_value=b'{}'), self.assertRaisesRegex(ValueError, "version manifest differs"):
            publish._verify_live("a" * 64, marker)


if __name__ == "__main__":
    unittest.main()
