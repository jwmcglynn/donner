"""Pins trusted CI routing to the configured self-hosted runner for each OS."""

import json
import os
import re
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

from python.runfiles import runfiles

from test_executable_fixtures import ExecutableFixtureDirectory


def _workflow_text(path):
    resolver = runfiles.Create()
    resolved = resolver.Rlocation("donner/%s" % path)
    with open(resolved, encoding="utf-8") as handle:
        return handle.read()


class CiRunnerRoutingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = _workflow_text(".github/workflows/main.yml")
        cls.trust_action = _workflow_text(".github/actions/pr-trust/action.yml")
        # The trust and routing scripts call python3 and gh; both resolve to fixtures here.
        cls.executables = ExecutableFixtureDirectory()
        cls.addClassCleanup(cls.executables.cleanup)
        cls.executables.write_python3()
        cls.executables.write("gh", "#!/bin/sh\nprintf 'jwmcglynn\\n'\n")

    def _job_body(self, job):
        marker = "\n  %s:\n" % job
        self.assertIn(marker, self.text, "job %s not found" % job)
        rest = self.text.split(marker, 1)[1]
        end = re.search(r"^  [A-Za-z0-9_-]+:\s*$", rest, re.MULTILINE)
        return rest[: end.start()] if end else rest

    def _routing_script(self):
        section = self.text.split("      - id: runner_gate\n", 1)[1]
        section = section.split("\n  determine-targets:\n", 1)[0]
        return textwrap.dedent(section.split("        run: |\n", 1)[1])

    def _trust_script(self):
        return textwrap.dedent(self.trust_action.split("      run: |\n", 1)[1])

    @staticmethod
    def _outputs(path):
        return dict(line.split("=", 1) for line in path.read_text().splitlines())

    def _route(self, *, event_name="pull_request", event=None,
               actor="jwmcglynn", triggering_actor="jwmcglynn",
               linux_enabled="true", macos_enabled="true"):
        if event is None:
            event = {
                "sender": {"login": "jwmcglynn"},
                "pull_request": {
                    "user": {"login": "jwmcglynn"},
                    "head": {"repo": {"full_name": "jwmcglynn/donner"}},
                    "base": {"repo": {"full_name": "jwmcglynn/donner"}, "ref": "main"},
                },
            }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            event_path = root / "event.json"
            output_path = root / "output"
            event_path.write_text(json.dumps(event), encoding="utf-8")
            env = dict(os.environ, ACTOR=actor, TRIGGERING_ACTOR=triggering_actor,
                       EVENT_NAME=event_name, REF="refs/heads/main",
                       REPOSITORY="jwmcglynn/donner", SHA="a" * 40,
                       SELFHOSTED_LINUX_RUNNER=linux_enabled,
                       SELFHOSTED_MACOS_RUNNER=macos_enabled,
                       GITHUB_EVENT_PATH=str(event_path), GITHUB_OUTPUT=str(output_path),
                       RUNNER_TEMP=directory, GH_TOKEN="test",
                       PATH=f"{self.executables.path}:{os.environ['PATH']}")
            # The gatekeeper classifies trust with the shared action, then routes on its output.
            trust_path = root / "trust"
            result = subprocess.run(["bash", "-c", self._trust_script()],
                                    env=dict(env, GITHUB_OUTPUT=str(trust_path)),
                                    text=True, capture_output=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            trust = self._outputs(trust_path)
            result = subprocess.run(["bash", "-c", self._routing_script()],
                                    env=dict(env, TRUSTED_PR=trust["trusted_pr"]),
                                    text=True, capture_output=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            return {**trust, **self._outputs(output_path)}

    def test_operator_pr_uses_self_hosted_lanes_when_enabled(self):
        route = self._route()
        self.assertEqual(route["use_self_hosted_linux"], "true")
        self.assertEqual(route["use_self_hosted_macos"], "true")

    def test_operator_stacked_pr_uses_self_hosted_lanes(self):
        event = {
            "sender": {"login": "jwmcglynn"},
            "pull_request": {
                "user": {"login": "jwmcglynn"},
                "head": {"repo": {"full_name": "jwmcglynn/donner"}},
                "base": {"repo": {"full_name": "jwmcglynn/donner"},
                         "ref": "feature/dependency"},
            },
        }
        route = self._route(event=event)
        self.assertEqual(route["use_self_hosted_linux"], "true")
        self.assertEqual(route["use_self_hosted_macos"], "true")

    def test_operator_main_push_uses_hosted_lanes(self):
        event = {"sender": {"login": "jwmcglynn"}}
        route = self._route(event_name="push", event=event)
        self.assertEqual(route["use_self_hosted_linux"], "false")
        self.assertEqual(route["use_self_hosted_macos"], "false")

    def test_operator_fork_pr_stays_hosted(self):
        event = {
            "sender": {"login": "jwmcglynn"},
            "pull_request": {
                "user": {"login": "jwmcglynn"},
                "head": {"repo": {"full_name": "jwmcglynn/fork"}},
                "base": {"repo": {"full_name": "jwmcglynn/donner"}, "ref": "main"},
            },
        }
        self.assertEqual(self._route(event=event)["use_self_hosted_linux"], "false")

    def test_missing_author_and_collaborator_rerun_stay_hosted(self):
        event = {
            "sender": {"login": "jwmcglynn"},
            "pull_request": {
                "user": {},
                "head": {"repo": {"full_name": "jwmcglynn/donner"}},
                "base": {"repo": {"full_name": "jwmcglynn/donner"}, "ref": "main"},
            },
        }
        self.assertEqual(self._route(event=event)["use_self_hosted_linux"], "false")
        self.assertEqual(self._route(triggering_actor="collaborator")[
            "use_self_hosted_linux"], "false")

    def test_disabled_linux_switch_only_affects_linux(self):
        route = self._route(linux_enabled="false")
        self.assertEqual(route["use_self_hosted_linux"], "false")
        self.assertEqual(route["use_self_hosted_macos"], "true")

    def test_operator_hosted_macos_label_preserves_linux_routing(self):
        event = {
            "sender": {"login": "jwmcglynn"},
            "pull_request": {
                "user": {"login": "jwmcglynn"},
                "head": {"repo": {"full_name": "jwmcglynn/donner"}},
                "base": {"repo": {"full_name": "jwmcglynn/donner"}, "ref": "main"},
                "labels": [{"name": "ci:hosted-macos"}],
            },
        }
        route = self._route(event=event)
        self.assertEqual(route["use_self_hosted_linux"], "true")
        self.assertEqual(route["use_self_hosted_macos"], "false")

        event["pull_request"]["base"]["ref"] = "feature/dependency"
        self.assertEqual(self._route(event=event)["use_self_hosted_macos"], "false")

    def test_hosted_macos_label_requires_exact_name_and_trusted_operator(self):
        event = {
            "sender": {"login": "jwmcglynn"},
            "pull_request": {
                "user": {"login": "jwmcglynn"},
                "head": {"repo": {"full_name": "jwmcglynn/donner"}},
                "base": {"repo": {"full_name": "jwmcglynn/donner"}, "ref": "main"},
                "labels": [{"name": "ci:hosted-macos-extra"}],
            },
        }
        self.assertEqual(self._route(event=event)["use_self_hosted_macos"], "true")
        event["pull_request"]["labels"] = [{"name": "ci:hosted-macos"}]
        event["pull_request"]["head"]["repo"]["full_name"] = "jwmcglynn/fork"
        fork = self._route(event=event)
        self.assertEqual(fork["use_self_hosted_linux"], "false")
        self.assertEqual(fork["use_self_hosted_macos"], "false")
        event["pull_request"]["head"]["repo"]["full_name"] = "jwmcglynn/donner"
        collaborator = self._route(event=event, triggering_actor="collaborator")
        self.assertEqual(collaborator["use_self_hosted_linux"], "false")
        self.assertEqual(collaborator["use_self_hosted_macos"], "false")

    def test_trust_rule_separates_maintainer_renovate_and_fork_pull_requests(self):
        """BuildBuddy write access and self-hosted routing share this one classification."""
        maintainer = self._route()
        self.assertEqual(("true", "false"), (maintainer["trusted_pr"], maintainer["renovate_pr"]))
        renovate = {
            "sender": {"login": "renovate[bot]"},
            "pull_request": {
                "user": {"login": "renovate[bot]"},
                "head": {"repo": {"full_name": "jwmcglynn/donner"}},
                "base": {"repo": {"full_name": "jwmcglynn/donner"}, "ref": "main"},
            },
        }
        route = self._route(event=renovate, actor="renovate[bot]",
                            triggering_actor="renovate[bot]")
        self.assertEqual(("false", "true"), (route["trusted_pr"], route["renovate_pr"]))
        # A maintainer rerun of a Renovate pull request stays Renovate, never trusted.
        rerun = self._route(event=renovate)
        self.assertEqual(("false", "true"), (rerun["trusted_pr"], rerun["renovate_pr"]))
        self.assertEqual("false", rerun["use_self_hosted_linux"])
        fork = dict(renovate, pull_request=dict(
            renovate["pull_request"], head={"repo": {"full_name": "someone/donner"}}))
        route = self._route(event=fork, actor="someone", triggering_actor="someone")
        self.assertEqual(("false", "false"), (route["trusted_pr"], route["renovate_pr"]))
        push = self._route(event_name="push", event={"sender": {"login": "jwmcglynn"}})
        self.assertEqual(("false", "false"), (push["trusted_pr"], push["renovate_pr"]))

    def test_gatekeeper_exports_the_shared_trust_rule(self):
        gatekeeper = self._job_body("gatekeeper")
        self.assertIn("      - id: pr_trust\n        name: Classify pull request trust\n"
                      "        uses: ./.github/actions/pr-trust\n", gatekeeper)
        self.assertIn("      trusted_pr: ${{ steps.pr_trust.outputs.trusted_pr }}\n", gatekeeper)
        self.assertIn("      renovate_pr: ${{ steps.pr_trust.outputs.renovate_pr }}\n", gatekeeper)
        self.assertIn("TRUSTED_PR: ${{ steps.pr_trust.outputs.trusted_pr }}", gatekeeper)

    def test_change_size_does_not_override_trusted_runner_routing(self):
        """A trusted run follows the runner gate regardless of changed-file count."""
        self.assertNotIn("SELF_HOSTED_MAX_CHANGED_FILES", self.text)
        self.assertNotIn("outputs.large_change", self.text)
        self.assertNotIn("changed_file_count", self.text)

    def test_linux_jobs_are_selected_only_by_the_trusted_runner_gate(self):
        hosted = self._job_body("linux")
        turnstile = self._job_body("linux-self-hosted-turnstile")
        self_hosted = self._job_body("linux-self-hosted")

        admission = hosted.split("    steps:\n", 1)[0]
        self.assertIn("outputs.use_self_hosted_linux != 'true'", admission)
        self.assertIn("outputs.use_self_hosted_linux == 'true'", turnstile)
        self.assertIn("outputs.use_self_hosted_linux == 'true'", self_hosted)

    def test_macos_jobs_are_selected_only_by_the_trusted_runner_gate(self):
        hosted = self._job_body("macos")
        self_hosted = self._job_body("macos-self-hosted")

        self.assertIn("outputs.use_self_hosted_macos != 'true'", hosted)
        self.assertIn("outputs.use_self_hosted_macos == 'true'", self_hosted)
        # The hosted-parity subset runs on the same runner, behind the same gate.
        self.assertIn("outputs.use_self_hosted_macos == 'true'",
                      self._job_body("macos-hosted-parity"))

    def test_hosted_browser_subset_follows_the_macos_runner_gate(self):
        # When the self-hosted macOS lane replaces the hosted job on a trusted PR, a hosted job
        # still runs main's browser step if the PR reaches a browser test. It never runs when
        # the hosted job runs anyway (main pushes, untrusted PRs, the ci:hosted-macos label).
        subset = self._job_body("macos-browser")
        condition = subset.split("    if: ", 1)[1].split("\n", 1)[0]
        self.assertIn("    runs-on: xcode-27\n", subset)
        self.assertIn("github.event_name == 'pull_request'", condition)
        self.assertIn("needs.gatekeeper.outputs.use_self_hosted_macos == 'true'", condition)
        self.assertIn("(needs.determine-targets.outputs.fallback == 'true' || "
                      "needs.determine-targets.outputs.browser_affected == 'true')", condition)
        self.assertNotIn("labels", condition)


if __name__ == "__main__":
    unittest.main()
