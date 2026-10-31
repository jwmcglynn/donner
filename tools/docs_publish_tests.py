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
        execution = mock.patch.dict(os.environ, {"GITHUB_SHA": "b" * 40})
        execution.start()
        self.addCleanup(execution.stop)

    def test_attestation_failures_keep_bounded_redacted_diagnostics(self):
        tokens = {"GH_TOKEN": "example-gh-secret", "GITHUB_TOKEN": "example-github-secret",
                  "ACTIONS_ID_TOKEN_REQUEST_TOKEN": "example-oidc-secret"}
        diagnostic = ("verification rejected " + " ".join(tokens.values()) +
                      "\n::error::untrusted response " + "x" * 6000).encode()
        error = publish.subprocess.CalledProcessError(7, ["gh", "attestation", "verify"],
                                                     stderr=diagnostic)
        bundle = self.root / "bundle.json"
        bundle.write_text("{}")
        calls = {
            "online": lambda: publish.attestation(self.root / "subject", publish.WORKFLOW,
                              "b" * 40, self.root / "online.json"),
            "bundle": lambda: publish._bundle_attestation(self.root / "subject", bundle,
                              "b" * 40, self.root / "bundle-proof.json"),
            "retained": lambda: publish._offline_attestation(self.root / "subject",
                site.canonical([{"attestation": {"bundle": {}}}]), "b" * 40, self.root),
        }
        for name, call in calls.items():
            with self.subTest(name=name), mock.patch.dict(os.environ, tokens), \
                 mock.patch.object(publish.subprocess, "run", side_effect=error):
                with self.assertRaisesRegex(ValueError, "attestation verification failed.*exit 7") as caught:
                    call()
                message = str(caught.exception)
                self.assertIn("verification rejected", message)
                self.assertIn("[REDACTED]", message)
                self.assertNotIn("\n", message)
                self.assertLess(len(message), 5000)
                for token in tokens.values():
                    self.assertNotIn(token, message)

    def test_attestation_diagnostic_redacts_overlapping_tokens_longest_first(self):
        tokens = {"GH_TOKEN": "example-token", "GITHUB_TOKEN": "example-token-extended",
                  "ACTIONS_ID_TOKEN_REQUEST_TOKEN": "example-token-extended-longest"}
        error = publish.subprocess.CalledProcessError(7, ["gh"],
            stderr=" ".join(tokens.values()).encode())
        with mock.patch.dict(os.environ, tokens), \
             mock.patch.object(publish.subprocess, "run", side_effect=error), \
             self.assertRaises(ValueError) as caught:
            publish.attestation(self.root / "subject", publish.WORKFLOW,
                                "b" * 40, self.root / "overlap.json")
        self.assertTrue(str(caught.exception).endswith(json.dumps("[REDACTED] [REDACTED] [REDACTED]")))

    def test_attestation_diagnostics_neutralize_legacy_actions_commands(self):
        diagnostic = "certificate error ##[error]text ##[add-mask]value ##[stop-commands]marker"
        error = publish.subprocess.CalledProcessError(7, ["gh"], stderr=diagnostic.encode())
        with mock.patch.object(publish.subprocess, "run", side_effect=error), \
             self.assertRaises(ValueError) as caught:
            publish.attestation(self.root / "subject", publish.WORKFLOW,
                                "b" * 40, self.root / "legacy.json")
        message = str(caught.exception)
        self.assertNotIn("##[", message)
        self.assertEqual(json.loads(message.split(": ", 1)[1]), diagnostic)

    def test_request_cannot_override_workflow_context_or_smuggle_extra_fields(self):
        request = {"composition_run": "123/1", "composition_sha256": "a" * 64, "prior_composition": "b" * 64, "bootstrap": None}
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

    def approval(self):
        return {"recovery_archive_sha256": "a" * 64, "observed_index_sha256": "b" * 64,
                "approve_initial_release_recovery": True}

    def test_bootstrap_requires_exact_digests_and_literal_approval(self):
        good = self.approval()
        publish.validate_bootstrap(good)
        for change in ({"approve_initial_release_recovery": False},
                       {"approve_initial_release_recovery": "true"},
                       {"recovery_archive_sha256": "latest"}, {"observed_index_sha256": ""},
                       {"extra": "unreviewed"}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                publish.validate_bootstrap({**good, **change})

    def test_standalone_composition_does_not_read_or_replace_an_existing_store(self):
        from tools import docs_fixture
        inputs = self.root / "inputs"
        inputs.mkdir()
        source = docs_fixture.source(inputs, "release")
        work = self.root / "standalone"

        def release_source(reference, digest, output):
            (output / "release-candidate.json").write_text("{}")
            (output / "record-attestation.json").write_text("[]")
            return source

        with mock.patch.object(publish, "release_source", side_effect=release_source), \
             mock.patch.object(publish, "main_source", return_value=source):
            publish.prepare(work, "1/1", "2/1", "a" * 64, None, self.api)
        selected = site.read_json(work / "payload/selection.json")
        self.assertIsNone(selected["base_snapshot_commit"])
        self.assertIsNone(selected["previous_composition"])
        self.assertIsNone(selected["bootstrap"])
        self.assertEqual(set(selected["releases"]), {"0.8.0"})
        self.assertEqual(selected["source_inputs"], {"main": source[1], "0.8.0": source[1]})
        self.assertEqual(site.verify_site(work / "site")["manifest_sha256"],
                         site.digest((work / "site/_docs/versions.json").read_bytes()))
        self.assertEqual(self.api.mock_calls, [])

    def test_standalone_composition_still_cannot_publish_over_an_existing_store(self):
        record = {**self.record, "base_snapshot_commit": None, "previous_composition": None}
        with self.assertRaisesRegex(ValueError, "snapshot branch advanced"):
            publish._preflight_record(record, None, self.root / "blocked", self.api)
        self.api.file.assert_not_called()
        self.api.call.assert_not_called()

    def test_bound_composition_still_requires_bootstrap_for_an_unactivated_seed(self):
        state = {"schema": 1, "releases": {"0.8.0": {}}, "current": None, "previous": None}
        self.api.file.return_value = site.canonical(state)
        with mock.patch.object(publish, "release_source") as release, \
             mock.patch.object(publish, "main_source") as main, \
             self.assertRaisesRegex(ValueError, "unactivated seed"):
            publish.prepare(self.root / "bound", "1/1", "2/1", "a" * 64,
                            "e" * 40, self.api)
        release.assert_not_called()
        main.assert_not_called()
        self.api.call.assert_not_called()

    def test_seed_is_only_allowed_before_the_snapshot_branch_exists(self):
        with mock.patch.object(publish, "fetch_composition") as fetch, self.assertRaisesRegex(ValueError, "before the snapshot"):
            publish.seed_recovery("1/1", "c" * 64, self.approval(), self.root / "seed", self.api)
        fetch.assert_not_called()
        self.api.call.assert_not_called()

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

    def test_reused_sha_status_cannot_authorize_recovery_of_an_inflight_candidate(self):
        self.api.call.return_value = {"status": "succeed"}
        with mock.patch.object(publish, "_live_identity", return_value=False), \
             mock.patch.object(publish, "live_marker", return_value={"manifest_sha256": "a" * 64}), \
             mock.patch.object(publish, "_wait_for_live"), \
             mock.patch.object(publish, "deploy") as deploy:
            result = publish._recover(self.record, self.prior, 7, self.api, "unhealthy candidate",
                                      "e" * 40, self.record["workflow_commit"])
        self.assertEqual(result["status"], "recovery-required")
        self.assertIn("not bound to the current artifact request", result["recovery_error"])
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

    def publication_plan(self):
        work, record, plan, commit, restore = self.transaction()
        plan.update(record=record, previous_archive_sha256=record["archive_sha256"])
        (work / "preflight.json").write_bytes(site.canonical(plan))
        return work, record, plan, commit, restore

    def publication_uploads(self, work, execution):
        import shutil
        import zipfile
        uploads, metadata = {}, {}
        for artifact_id, role in ((9, "rollback"), (11, "candidate")):
            uploads[artifact_id] = work / (role + "-upload.zip")
            with zipfile.ZipFile(uploads[artifact_id], "w") as zipped:
                zipped.write(work / "pages/artifact.tar", "artifact.tar")
            metadata[artifact_id] = {"expired": False, "name": "docs-" + role + "-1",
                "digest": "sha256:" + publish.bcr_source.digest(uploads[artifact_id]),
                "workflow_run": {"id": 123, "head_sha": execution}}
        def download(artifact_id, output):
            shutil.copyfile(uploads[artifact_id], output)
        return metadata, download

    def test_normal_promotion_uses_current_upload_and_execution_sha(self):
        work, record, plan, commit, restore = self.publication_plan()
        execution = "c" * 40
        metadata, download = self.publication_uploads(work, execution)
        environment = {"GITHUB_RUN_ID": "123", "GITHUB_RUN_ATTEMPT": "1",
                       "GITHUB_SHA": execution, "VERIFIED_CANDIDATE_ARTIFACT_ID": "11"}
        with mock.patch.dict(os.environ, environment), \
             mock.patch.object(publish.store, "index", return_value=plan["current"]), \
             mock.patch.object(publish, "_live_identity", return_value=True), \
             mock.patch.object(publish, "github_json", side_effect=lambda path: metadata[int(path.rsplit("/", 1)[1])]), \
             mock.patch.object(publish, "download", side_effect=download), \
             mock.patch.object(publish.store, "commit", side_effect=commit), \
             mock.patch.object(publish.store, "restore", side_effect=restore), \
             mock.patch.object(publish, "_wait_for_live"), \
             mock.patch.object(publish, "deploy", return_value="91") as deploy:
            result = publish.promote(work, 9, self.api)
        self.assertEqual(result["status"], "published")
        self.assertEqual(deploy.call_args.args[:2], (11, execution))
        self.assertEqual(record["artifact_id"], 7)
        self.assertEqual(record["workflow_commit"], "b" * 40)

    def test_promotion_consumers_reject_candidate_scope_before_retention(self):
        work, record, plan, _, _ = self.publication_plan()
        wrong_attempt = {"expired": False, "name": "docs-candidate-0",
                         "workflow_run": {"id": 123, "head_sha": "b" * 40}}
        for event in ("workflow_dispatch", "workflow_run"):
            with self.subTest(event=event), \
                 mock.patch.dict(os.environ, {"GITHUB_EVENT_NAME": event, "GITHUB_RUN_ID": "123",
                     "GITHUB_RUN_ATTEMPT": "1", "VERIFIED_CANDIDATE_ARTIFACT_ID": "11"}), \
                 mock.patch.object(publish.store, "index", return_value=plan["current"]), \
                 mock.patch.object(publish, "_live_identity", return_value=True), \
                 mock.patch.object(publish, "_verified_previous"), \
                 mock.patch.object(publish, "github_json", return_value=wrong_attempt), \
                 mock.patch.object(publish, "_retain_and_deploy") as retain:
                with self.assertRaisesRegex(ValueError, "candidate upload is not from this trusted run"):
                    publish.promote(work, 9, self.api)
                retain.assert_not_called()
                self.api.call.assert_not_called()

    def test_recovery_uses_execution_sha_without_changing_signed_source(self):
        execution = "c" * 40
        with mock.patch.dict(os.environ, {"GITHUB_SHA": execution}), \
             mock.patch.object(publish, "_live_identity", return_value=False), \
             mock.patch.object(publish, "live_marker", return_value={"manifest_sha256": "a" * 64}), \
             mock.patch.object(publish, "_wait_for_live"), \
             mock.patch.object(publish, "deploy", return_value="92") as deploy:
            result = publish._recover(self.record, self.prior, 7, self.api, "unhealthy", "e" * 40, "91")
        self.assertEqual(result["status"], "restored")
        self.assertEqual(deploy.call_args.args[:2], (7, execution))
        self.assertEqual(self.record["workflow_commit"], "b" * 40)

    def test_transport_rejection_blocks_retention_for_candidate_and_recovery(self):
        import io
        import shutil
        import tarfile
        import zipfile
        original_root = self.root
        for invalid in ("candidate", "recovery"):
            with self.subTest(invalid=invalid):
                self.root = original_root / invalid
                self.root.mkdir()
                work, record, plan, commit, restore = self.transaction()
                previous = work / "previous.tar"
                site.pack(work / "site", previous)
                bad = work / "pages/artifact.tar" if invalid == "candidate" else previous
                with tarfile.open(bad, "w:") as tar:
                    info = tarfile.TarInfo("index.html")
                    info.mode, info.size = 0o644, 5
                    tar.addfile(info, io.BytesIO(b"prior"))
                record["archive_sha256"] = publish.bcr_source.digest(work / "pages/artifact.tar")
                plan.update(record=record, previous_archive_sha256=publish.bcr_source.digest(previous))
                (work / "preflight.json").write_bytes(site.canonical(plan))
                uploaded = work / "uploaded.zip"
                with zipfile.ZipFile(uploaded, "w") as zipped:
                    zipped.write(previous, "artifact.tar")
                meta = {"expired": False, "name": "docs-rollback-1",
                        "digest": "sha256:" + publish.bcr_source.digest(uploaded),
                        "workflow_run": {"id": 123, "head_sha": "b" * 40}}
                def download(artifact_id, output):
                    shutil.copyfile(uploaded, output)
                with mock.patch.dict(os.environ, {"GITHUB_RUN_ID": "123", "GITHUB_RUN_ATTEMPT": "1",
                                                  "GITHUB_SHA": "b" * 40}), \
                     mock.patch.object(publish.store, "index", return_value=plan["current"]), \
                     mock.patch.object(publish, "_live_identity", return_value=True), \
                     mock.patch.object(publish, "github_json", return_value=meta), \
                     mock.patch.object(publish, "download", side_effect=download), \
                     mock.patch.object(publish, "_retain_and_deploy") as retain, \
                     self.assertRaisesRegex(ValueError, "must start with ./"):
                    publish.promote(work, 9, self.api)
                retain.assert_not_called()
                self.api.call.assert_not_called()
                shutil.rmtree(work)

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
        api.file.return_value = site.canonical({"archive_sha256": "b" * 64})
        with self.assertRaisesRegex(ValueError, "content address differs"):
            publish._previous(api, "a" * 40, "c" * 64, self.root)

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

    def recovery_fixture(self):
        from tools import docs_fixture
        work = self.root / "recovery"
        work.mkdir()
        source = docs_fixture.source(work, "release")
        site.compose({"0.8.0": source, "main": source}, "0.8.0", work / "site")
        (work / "pages").mkdir()
        site.pack(work / "site", work / "pages/artifact.tar")
        payload = work / "payload"
        payload.mkdir()
        entry = {"identity": source[1], "archive": publish.store.split_archive(source[0], payload)}
        selected = {"releases": {"0.8.0": entry}, "bootstrap": None,
                    "source_inputs": {"0.8.0": source[1], "main": source[1]}}
        (payload / "selection.json").write_bytes(site.canonical(selected))
        record = {"archive_sha256": publish.bcr_source.digest(work / "pages/artifact.tar"),
                  "manifest_sha256": site.verify_site(work / "site")["manifest_sha256"],
                  "workflow_commit": "c" * 40, "base_snapshot_commit": None,
                  "previous_composition": None, "bootstrap": None, "payload_files": site.inventory(payload)}
        (payload / "composition.json").write_bytes(site.canonical(record))
        for name in ("composition-attestation.json", "pages-attestation.json"):
            (work / name).write_text('[]')
        durable, _ = publish._retain_composition(record, work)
        approved = {**self.approval(), "recovery_archive_sha256": record["archive_sha256"]}
        (durable / "compositions" / record["archive_sha256"] / "bootstrap.json").write_bytes(site.canonical(approved))
        api = mock.Mock()
        api.file.side_effect = lambda commit, name: (durable / name).read_bytes()
        current = {"schema": 1, "releases": selected["releases"], "current": None, "previous": None}
        return work, record, selected, durable, approved, api, current

    def test_bootstrap_restores_both_attested_bytes_and_revalidates_release_source(self):
        work, record, selected, durable, approved, api, current = self.recovery_fixture()
        with mock.patch.object(publish, "_offline_attestation") as attest, \
             mock.patch.object(publish, "_verify_retained_release") as source:
            identity = publish._bootstrap_recovery(api, "e" * 40, current, approved, self.root / "verified")
        self.assertEqual(identity["marker"], site.verify_site(work / "site"))
        self.assertEqual(attest.call_count, 2)
        self.assertEqual(source.call_count, 1)
        api.call.assert_not_called()

    def test_bootstrap_cannot_be_reused_after_either_activation(self):
        for active in ("a" * 64, "c" * 64):
            with self.subTest(active=active), self.assertRaisesRegex(ValueError, "before a composition is activated"):
                publish._bootstrap_recovery(self.api, "d" * 40,
                    {"current": active, "previous": None}, self.approval(), self.root / "unused")
        self.api.file.assert_not_called()

    def test_bootstrap_rejects_seed_drift_forged_selection_and_corrupt_stored_bytes(self):
        work, record, selected, durable, approved, api, current = self.recovery_fixture()
        with self.assertRaisesRegex(ValueError, "immutable seed"):
            publish._bootstrap_recovery(api, "e" * 40, current,
                {**approved, "observed_index_sha256": "f" * 64}, self.root / "drift")
        prefix = durable / "compositions" / record["archive_sha256"]
        selection = prefix / "selection.json"
        original = selection.read_bytes()
        selection.write_bytes(site.canonical({**selected, "bootstrap": approved}))
        with mock.patch.object(publish, "_offline_attestation"), self.assertRaisesRegex(ValueError, "signed selection changed"):
            publish._bootstrap_recovery(api, "e" * 40, current, approved, self.root / "forged")
        selection.write_bytes(original)
        chunk = next((durable / "objects").rglob("*.part"))
        chunk.write_bytes(b"corrupt")
        with mock.patch.object(publish, "_offline_attestation"), \
             mock.patch.object(publish, "_verify_retained_release"), self.assertRaisesRegex(ValueError, "stored chunk changed"):
            publish._bootstrap_recovery(api, "e" * 40, current, approved, self.root / "corrupt")

    def test_recovery_main_must_equal_its_independently_signed_release(self):
        work, record, selected, durable, approved, api, current = self.recovery_fixture()
        forged = {**selected, "source_inputs": {**selected["source_inputs"], "main": {}}}
        with mock.patch.object(publish, "_verify_retained_release") as verify, \
             self.assertRaisesRegex(ValueError, "same signed release source"):
            publish._recovery_sources(record, forged, work, api, "e" * 40)
        verify.assert_not_called()

    def test_first_publication_request_must_match_the_signed_bootstrap_selection(self):
        record = {**self.record, "base_snapshot_commit": "c" * 40, "previous_composition": None,
                  "bootstrap": self.approval()}
        with mock.patch.object(publish, "fetch_composition", return_value=record), \
             mock.patch.object(publish.store, "index", return_value={"current": None}), \
             mock.patch.object(publish, "_bootstrap_recovery") as recovery, \
             self.assertRaisesRegex(ValueError, "signed bootstrap selection"):
            publish.preflight("1/1", "e" * 64, None, self.root / "preflight", self.api)
        recovery.assert_not_called()

    def test_seed_never_claims_recovery_site_is_already_live(self):
        work, record, selected, durable, approved, api, current = self.recovery_fixture()
        api.tip.return_value = None
        target = self.root / "seed"
        def fetch(reference, expected, output):
            import shutil
            for name in ("site", "pages", "payload"):
                shutil.copytree(work / name, output / name)
            for name in ("composition-attestation.json", "pages-attestation.json"):
                shutil.copyfile(work / name, output / name)
            return record
        with mock.patch.object(publish, "fetch_composition", side_effect=fetch), \
             mock.patch.object(publish, "_verify_retained_release"), \
             mock.patch.object(publish, "_live_identity", return_value=True), \
             mock.patch.object(publish.store, "commit", return_value="e" * 40) as commit, \
             mock.patch.object(publish, "deploy") as deploy:
            publish.seed_recovery("1/1", "d" * 64, approved, target, api)
        self.assertIsNone(commit.call_args.args[3]["current"])
        self.assertIsNone(commit.call_args.args[3]["previous"])
        deploy.assert_not_called()

    def test_bootstrap_recovery_verifies_its_distinct_destination(self):
        fallback = {"kind": "composition", "marker": {"manifest_sha256": "f" * 64}}
        with mock.patch.object(publish, "_live_identity", return_value=False), \
             mock.patch.object(publish, "live_marker", return_value={"manifest_sha256": "a" * 64}), \
             mock.patch.object(publish, "deploy", return_value="92"), \
             mock.patch.object(publish, "_wait_for_live") as verify:
            result = publish._recover(self.record, {"kind": "legacy", "index_sha256": "e" * 64},
                7, self.api, "unhealthy", "e" * 40, "91", recovery_identity=fallback)
        self.assertEqual(result["status"], "restored")
        verify.assert_called_once_with(fallback)

    def test_failed_first_cutover_preserves_seed_roster_for_a_retry(self):
        work, record, plan, commit, restore = self.transaction()
        record["bootstrap"] = self.approval()
        selected = {"releases": {"0.9.0": {"identity": "newly retained source"}}}
        (work / "payload/selection.json").write_bytes(site.canonical(selected))
        record["payload_files"] = site.inventory(work / "payload")
        record["payload_files"].pop("composition.json")
        (work / "payload/composition.json").write_bytes(site.canonical(record))
        with mock.patch.object(publish.store, "commit", side_effect=commit) as save, \
             mock.patch.object(publish.store, "restore", side_effect=restore), \
             mock.patch.object(publish, "deploy", side_effect=ValueError("rejected before cutover")), \
             mock.patch.object(publish, "_recover", return_value={"status": "unchanged"}), \
             self.assertRaisesRegex(ValueError, "inspect result.json"):
            publish._retain_and_deploy(record, plan, 9, work, self.api)
        self.assertEqual(save.call_count, 1)
        self.assertEqual(save.call_args.args[3]["releases"], plan["current"]["releases"])
        self.assertEqual(site.read_json(work / "result.json")["status"], "unchanged")

    def test_only_healthy_bootstrap_recovery_activates_its_pointer_truthfully(self):
        for failed_activation in (False, True):
            with self.subTest(failed_activation=failed_activation):
                work, record, plan, commit, restore = self.transaction()
                record["bootstrap"] = self.approval()
                (work / "payload/composition.json").write_bytes(site.canonical(record))
                commits = ["e" * 40, ValueError("lost response") if failed_activation else "f" * 40]
                with mock.patch.object(publish.store, "commit", side_effect=commits) as save, \
                     mock.patch.object(publish.store, "restore", side_effect=restore), \
                     mock.patch.object(publish, "deploy", side_effect=ValueError("unhealthy")), \
                     mock.patch.object(publish, "_recover", return_value={"status": "restored"}), \
                     self.assertRaisesRegex(ValueError, "inspect result.json"):
                    publish._retain_and_deploy(record, plan, 9, work, self.api)
                result = site.read_json(work / "result.json")
                self.assertEqual(result["status"], "recovery-required" if failed_activation else "recovered-to-bootstrap")
                self.assertEqual(save.call_args.args[3]["current"], "a" * 64)
                self.assertIsNone(save.call_args.args[3]["previous"])
                import shutil
                shutil.rmtree(work)

    def test_health_wait_allows_public_propagation_without_entering_recovery(self):
        work, record, plan, commit, restore = self.transaction()
        with mock.patch.object(publish.store, "commit", side_effect=commit), \
             mock.patch.object(publish.store, "restore", side_effect=restore), \
             mock.patch.object(publish, "deploy", return_value="91"), \
             mock.patch.object(publish, "_verify_live", side_effect=[ValueError("old site"), None]), \
             mock.patch.object(publish.time, "sleep") as pause, \
             mock.patch.object(publish, "_recover", return_value={"status": "recovery-required"}) as recover:
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

    def test_slow_health_check_exhausts_elapsed_budget_without_more_attempts(self):
        clock = [0.0]
        def slow_check(expected, **kwargs):
            clock[0] += 121
            return False
        with mock.patch.object(publish.time, "monotonic", side_effect=lambda: clock[0]), \
             mock.patch.object(publish, "_live_identity", side_effect=slow_check) as check, \
             self.assertRaisesRegex(ValueError, "public site did not converge"):
            pause = mock.Mock()
            publish._wait_for_live(self.prior, wait=pause)
        self.assertEqual(check.call_count, 1)
        pause.assert_not_called()

    def test_expired_health_budget_stops_before_a_network_request(self):
        with mock.patch.object(publish.time, "monotonic", return_value=121), \
             mock.patch.object(publish, "_https_response") as request, \
             self.assertRaisesRegex(ValueError, "deadline"):
            publish._live_file("index.html", deadline=120)
        request.assert_not_called()

    def test_live_read_checks_deadline_between_chunks(self):
        clock = [0.0]
        response = mock.Mock()
        def slow_chunk(size):
            clock[0] += 121
            return b"partial"
        response.read1.side_effect = slow_chunk
        with mock.patch.object(publish.time, "monotonic", side_effect=lambda: clock[0]), \
             self.assertRaisesRegex(ValueError, "deadline"):
            publish._read_body(response, 100, deadline=120)
        self.assertEqual(response.read1.call_count, 1)

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

    def automatic_environment(self):
        event = self.root / "event.json"
        event.write_bytes(site.canonical({"workflow_run": {"id": 123, "run_attempt": 1}}))
        return {"GITHUB_REPOSITORY": "jwmcglynn/donner", "GITHUB_REF": "refs/heads/main",
                "GITHUB_EVENT_NAME": "workflow_run", "GITHUB_SHA": "b" * 40,
                "GITHUB_WORKFLOW_REF": "jwmcglynn/donner/.github/workflows/deploy_docs.yaml@refs/heads/main",
                "GITHUB_RUN_ID": "456", "GITHUB_RUN_ATTEMPT": "1", "GITHUB_EVENT_PATH": str(event)}

    def test_automatic_source_is_verified_through_github_not_webhook_claims(self):
        run = {"id": 123, "run_attempt": 1, "repository": {"full_name": "jwmcglynn/donner"},
               "head_repository": {"full_name": "jwmcglynn/donner"}, "path": publish.WORKFLOW,
               "event": "push", "head_branch": "main", "head_sha": "c" * 40,
               "status": "completed", "conclusion": "success"}
        with mock.patch.dict(os.environ, self.automatic_environment()), \
             mock.patch.object(publish, "github_json", return_value=run) as fetch:
            self.assertEqual(publish._automatic_trigger()["source_commit"], "c" * 40)
            self.assertTrue(fetch.call_args.args[0].endswith("/123/attempts/1"))
        for change in ({"event": "pull_request"}, {"event": "workflow_run"},
                       {"head_branch": "feature"}, {"conclusion": "failure"},
                       {"head_repository": {"full_name": "fork/donner"}}, {"run_attempt": 2}):
            with self.subTest(change=change), mock.patch.dict(os.environ, self.automatic_environment()), \
                 mock.patch.object(publish, "github_json", return_value={**run, **change}), \
                 self.assertRaises(ValueError):
                publish._automatic_trigger()

    def test_automatic_context_cannot_select_an_environment_from_another_workflow(self):
        good = self.automatic_environment()
        with mock.patch.dict(os.environ, good):
            self.assertEqual(publish.store.deployment_environment(), "github-pages-main")
        for key, value in (("GITHUB_WORKFLOW_REF", "fork/donner/evil.yml@refs/heads/main"),
                           ("GITHUB_REF", "refs/pull/1/merge"), ("GITHUB_REPOSITORY", "fork/donner")):
            with self.subTest(key=key), mock.patch.dict(os.environ, {**good, key: value}), self.assertRaises(ValueError):
                publish.store.deployment_environment()

    def test_automatic_source_order_requires_ancestry_and_skips_old_or_identical_sources(self):
        old, new = "a" * 40, "b" * 40
        with mock.patch.object(publish, "github_json") as fetch:
            self.assertFalse(publish._source_order(old, old))
            fetch.assert_not_called()
        with mock.patch.object(publish, "github_json", return_value={"status": "behind"}):
            self.assertFalse(publish._source_order(old, new))
        with mock.patch.object(publish, "github_json", return_value={"status": "ahead", "merge_base_commit": {"sha": old}}):
            self.assertTrue(publish._source_order(old, new))
        for reply in ({"status": "diverged"}, {"status": "ahead", "merge_base_commit": {"sha": "c" * 40}}):
            with mock.patch.object(publish, "github_json", return_value=reply), self.assertRaisesRegex(ValueError, "does not descend"):
                publish._source_order(old, new)

    def test_automatic_preparation_never_initializes_the_store(self):
        self.api.tip.return_value = None
        with mock.patch.object(publish, "_automatic_trigger", return_value={}), \
             mock.patch.object(publish, "prepare") as prepare, \
             mock.patch.object(publish.store, "commit") as commit:
            publish.prepare_auto(self.root / "unused", self.api)
        prepare.assert_not_called()
        commit.assert_not_called()

    def test_newer_automatic_source_still_assembles_if_an_earlier_publication_advances(self):
        from tools import docs_fixture
        inputs = self.root / "inputs"
        inputs.mkdir()
        release = docs_fixture.source(inputs, "release")
        development = docs_fixture.source(inputs, "main")
        objects = inputs / "objects-payload"
        objects.mkdir()
        descriptor = publish.store.split_archive(release[0], objects)
        state = {"schema": 1, "current": "e" * 64, "previous": None,
                 "releases": {"0.8.0": {"identity": release[1], "archive": descriptor}}}
        self.api.tip.return_value = "a" * 40
        self.api.file.side_effect = lambda commit, name: (site.canonical(state) if name == "index.json"
                                                        else (objects / name).read_bytes())
        work = self.root / "auto-interleaving"
        def restore(api, snapshot, target, output):
            path = output / "site/_docs"
            path.mkdir(parents=True)
            manifest = {"versions": {"main": {"identity": {"source_commit": "c" * 40}}}}
            (path / "versions.json").write_bytes(site.canonical(manifest))
            self.api.tip.return_value = "b" * 40
            return {}, {}
        with mock.patch.dict(os.environ, self.automatic_environment()), \
             mock.patch.object(publish, "_automatic_trigger", return_value={"run_id": 123, "attempt": 1,
                                                                           "source_commit": "d" * 40}), \
             mock.patch.object(publish, "_restore_composition", side_effect=restore), \
             mock.patch.object(publish, "_verify_current_roster"), \
             mock.patch.object(publish, "_verify_retained_release"), \
             mock.patch.object(publish, "_source_order", return_value=True), \
             mock.patch.object(publish, "main_source", return_value=development):
            publish.prepare_auto(work, self.api)
        selected = site.read_json(work / "candidate/payload/selection.json")
        self.assertEqual(selected["base_snapshot_commit"], "a" * 40)
        self.assertEqual(self.api.tip.return_value, "b" * 40)
        site.verify_site(work / "candidate/site")
        self.api.call.assert_not_called()

    def automatic_selection(self):
        identity = {"version": "0.8.0", "source_commit": "c" * 40}
        releases = {"0.8.0": {"identity": identity, "archive": {"sha256": "d" * 64}}}
        current = {"schema": 1, "releases": releases, "current": "e" * 64, "previous": None}
        selected = {"releases": releases, "bootstrap": None, "base_snapshot_commit": "a" * 40,
                    "previous_composition": "f" * 64,
                    "source_inputs": {"0.8.0": identity, "main": {"source_commit": "b" * 40}}}
        record = {"bootstrap": None, "base_snapshot_commit": "a" * 40, "previous_composition": "f" * 64}
        manifest = {"versions": {"0.8.0": {"identity": identity}, "main": {"identity": {"source_commit": "a" * 40}}}}
        return current, selected, record, manifest

    def test_automatic_scope_cannot_add_change_or_activate_a_pending_release(self):
        import copy
        current, selected, record, manifest = self.automatic_selection()
        publish._automatic_scope(record, selected, current, manifest)
        bad_selected = copy.deepcopy(selected)
        bad_selected["releases"]["0.9.0"] = {"identity": {}}
        with self.assertRaisesRegex(ValueError, "release roster"):
            publish._automatic_scope(record, bad_selected, current, manifest)
        with self.assertRaisesRegex(ValueError, "pending manual release"):
            publish._automatic_scope(record, selected, current, {"versions": {"main": {}}})
        with self.assertRaisesRegex(ValueError, "cannot bootstrap"):
            publish._automatic_scope({**record, "bootstrap": self.approval()}, selected, current, manifest)

    def test_automatic_handoff_cannot_fetch_a_different_inprogress_run(self):
        with mock.patch.dict(os.environ, self.automatic_environment()), \
             mock.patch.object(publish, "github_json", return_value={"id": 455}), \
             mock.patch.object(publish, "_auto_artifact") as download, \
             self.assertRaisesRegex(ValueError, "not this trusted workflow run"):
            publish._fetch_auto_composition("a" * 64, self.root)
        download.assert_not_called()

    def test_pages_api_target_is_bound_to_the_manual_or_automatic_environment(self):
        for event, expected in (("workflow_dispatch", "github-pages"), ("workflow_run", "github-pages-main")):
            api = mock.Mock()
            api.call.side_effect = [{"id": "91", "page_url": publish.SITE_URL}, {"status": "succeed"}]
            with mock.patch.dict(os.environ, {**self.automatic_environment(), "GITHUB_EVENT_NAME": event}), \
                 mock.patch.object(publish, "_oidc_token", return_value="fixture-token"):
                self.assertEqual(publish.deploy(7, "b" * 40, api), "91")
            self.assertEqual(api.call.call_args_list[0].args[2]["environment"], expected)
            api.require_protection.assert_called_once()

    def test_pages_build_version_is_the_exact_source_commit(self):
        commit = "b" * 40
        for artifact_id in (7, 8):
            with self.subTest(artifact_id=artifact_id):
                api = mock.Mock()
                api.call.side_effect = [
                    {"id": commit, "page_url": publish.SITE_URL}, {"status": "succeed"}]
                receipt = self.root / (str(artifact_id) + ".json")
                with mock.patch.object(publish, "_oidc_token", return_value="fixture-token"):
                    self.assertEqual(publish.deploy(artifact_id, commit, api, receipt=receipt), commit)
                request = api.call.call_args_list[0].args[2]
                self.assertEqual(request["pages_build_version"], commit)
                self.assertEqual(request["artifact_id"], artifact_id)
                self.assertEqual(site.read_json(receipt), {
                    "deployment_id": commit, "artifact_id": artifact_id})

    def test_pages_rejects_a_non_commit_build_version_before_credentials_or_requests(self):
        for commit in ("b" * 40 + "-7", "main", "b" * 39, None):
            with self.subTest(commit=commit):
                api = mock.Mock()
                with mock.patch.object(publish, "_oidc_token") as token, \
                     self.assertRaisesRegex(ValueError, "invalid Pages source commit"):
                    publish.deploy(7, commit, api)
                token.assert_not_called()
                api.call.assert_not_called()

    def test_wrong_pages_url_preserves_the_known_request_identity_before_failure(self):
        api = mock.Mock()
        api.call.return_value = {"id": "91", "page_url": "https://other.test/"}
        receipt = self.root / "receipt.json"
        with mock.patch.object(publish, "_oidc_token", return_value="fixture-token"), \
             self.assertRaisesRegex(ValueError, "unexpected site URL"):
            publish.deploy(7, "b" * 40, api, receipt=receipt)
        self.assertEqual(site.read_json(receipt)["deployment_id"], "91")

    def test_automatic_preflight_rebinds_only_the_operational_plan_to_the_fresh_snapshot(self):
        current, selected, record, manifest = self.automatic_selection()
        work = self.root / "auto-preflight"
        def fetch(expected, output):
            (output / "payload").mkdir()
            (output / "payload/selection.json").write_bytes(site.canonical(selected))
            return record
        def previous(api, snapshot, prior, output):
            self.assertEqual(snapshot, "b" * 40)
            (output / "previous-pages").mkdir()
            (output / "previous-pages/artifact.tar").write_bytes(b"verified current complete site")
            path = output / "prior-proof/site/_docs"
            path.mkdir(parents=True)
            (path / "versions.json").write_bytes(site.canonical(manifest))
            return {}, self.prior
        self.api.tip.return_value = "b" * 40
        with mock.patch.object(publish, "_fetch_auto_composition", side_effect=fetch), \
             mock.patch.object(publish.store, "index", return_value=current), \
             mock.patch.object(publish, "_previous", side_effect=previous), \
             mock.patch.object(publish, "_verify_auto_source"), \
             mock.patch.object(publish, "_source_order", return_value=True), \
             mock.patch.object(publish, "_live_identity", return_value=True):
            publish.preflight_auto("c" * 64, work, self.api)
        plan = site.read_json(work / "preflight.json")
        self.assertEqual(plan["snapshot"], "b" * 40)
        self.assertEqual(plan["prior"], current["current"])
        self.assertEqual(plan["record"]["base_snapshot_commit"], "a" * 40)
        self.assertEqual(plan["record"]["previous_composition"], "f" * 64)
        self.assertTrue(plan["automatic"])

    def test_retention_and_activation_use_the_fresh_automatic_plan_cas(self):
        work, record, plan, commit, restore = self.transaction()
        plan.update(snapshot="c" * 40, automatic=True)
        selected = {"releases": {}, "source_inputs": {"main": {}}}
        (work / "payload/selection.json").write_bytes(site.canonical(selected))
        record["payload_files"] = site.inventory(work / "payload")
        record["payload_files"].pop("composition.json")
        (work / "payload/composition.json").write_bytes(site.canonical(record))
        path = work / "prior-proof/site/_docs"
        path.mkdir(parents=True)
        (path / "versions.json").write_text('{}')
        with mock.patch.dict(os.environ, self.automatic_environment()), \
             mock.patch.object(publish, "_automatic_scope"), mock.patch.object(publish, "_verify_auto_source"), \
             mock.patch.object(publish.store, "commit", side_effect=["e" * 40, "f" * 40]) as save, \
             mock.patch.object(publish.store, "restore", side_effect=restore), \
             mock.patch.object(publish, "deploy", return_value="91"), \
             mock.patch.object(publish, "_verify_live"):
            result = publish._retain_and_deploy(record, plan, 9, work, self.api)
        self.assertEqual(result["status"], "published")
        self.assertEqual(save.call_args_list[0].args[1], "c" * 40)
        self.assertEqual(save.call_args_list[1].args[1], "e" * 40)
        self.assertEqual(save.call_args_list[1].args[3]["previous"], plan["prior"])

    def test_pages_health_checks_detect_changed_version_index(self):
        marker = {"manifest_sha256": "a" * 64}
        with mock.patch.object(publish, "live_marker", return_value=marker), \
             mock.patch.object(publish, "_live_file", return_value=b'{}'), self.assertRaisesRegex(ValueError, "version manifest differs"):
            publish._verify_live("a" * 64, marker)


if __name__ == "__main__":
    unittest.main()
