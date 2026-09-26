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


def _workflow_text(path):
    resolver = runfiles.Create()
    resolved = resolver.Rlocation("donner/%s" % path)
    with open(resolved, encoding="utf-8") as handle:
        return handle.read()


class CiRunnerRoutingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = _workflow_text(".github/workflows/main.yml")
        cls.coverage_text = _workflow_text(".github/workflows/coverage.yml")

    def _job_body(self, job):
        marker = "\n  %s:\n" % job
        self.assertIn(marker, self.text, "job %s not found" % job)
        rest = self.text.split(marker, 1)[1]
        end = re.search(r"^  [A-Za-z0-9_-]+:\s*$", rest, re.MULTILINE)
        return rest[: end.start()] if end else rest

    def _routing_script(self, workflow):
        if workflow == "main":
            section = self.text.split("      - id: runner_gate\n", 1)[1]
            section = section.split("\n  determine-targets:\n", 1)[0]
        else:
            section = self.coverage_text.split("      - id: determine\n", 1)[1]
            section = section.split(
                '          # Single source of truth for "does a coverage lane actually run?".',
                1,
            )[0]
        return textwrap.dedent(section.split("        run: |\n", 1)[1])

    def _route(self, workflow, *, event_name="pull_request", event=None,
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
            gh = root / "gh"
            gh.write_text("#!/bin/sh\nprintf 'jwmcglynn\\n'\n", encoding="utf-8")
            gh.chmod(0o755)
            env = dict(os.environ, ACTOR=actor, TRIGGERING_ACTOR=triggering_actor,
                       EVENT_NAME=event_name, REF="refs/heads/main",
                       REPOSITORY="jwmcglynn/donner", SHA="a" * 40,
                       SELFHOSTED_LINUX_RUNNER=linux_enabled,
                       SELFHOSTED_MACOS_RUNNER=macos_enabled,
                       GITHUB_EVENT_PATH=str(event_path), GITHUB_OUTPUT=str(output_path),
                       RUNNER_TEMP=directory, GH_TOKEN="test", PATH=f"{directory}:{os.environ['PATH']}")
            result = subprocess.run(["bash", "-c", self._routing_script(workflow)],
                                    env=env, text=True, capture_output=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            return dict(line.split("=", 1) for line in output_path.read_text().splitlines())

    def test_operator_pr_uses_self_hosted_lanes_when_enabled(self):
        for workflow in ("main", "coverage"):
            with self.subTest(workflow=workflow):
                route = self._route(workflow)
                self.assertEqual(route["use_self_hosted_linux"], "true")
                if workflow == "main":
                    self.assertEqual(route["use_self_hosted_macos"], "true")

    def test_operator_main_push_uses_hosted_lanes(self):
        event = {"sender": {"login": "jwmcglynn"}}
        for workflow in ("main", "coverage"):
            with self.subTest(workflow=workflow):
                route = self._route(workflow, event_name="push", event=event)
                self.assertEqual(route["use_self_hosted_linux"], "false")
                if workflow == "main":
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
        for workflow in ("main", "coverage"):
            with self.subTest(workflow=workflow):
                self.assertEqual(self._route(workflow, event=event)["use_self_hosted_linux"],
                                 "false")

    def test_missing_author_and_collaborator_rerun_stay_hosted(self):
        event = {
            "sender": {"login": "jwmcglynn"},
            "pull_request": {
                "user": {},
                "head": {"repo": {"full_name": "jwmcglynn/donner"}},
                "base": {"repo": {"full_name": "jwmcglynn/donner"}, "ref": "main"},
            },
        }
        for workflow in ("main", "coverage"):
            with self.subTest(workflow=workflow):
                self.assertEqual(self._route(workflow, event=event)["use_self_hosted_linux"],
                                 "false")
                self.assertEqual(self._route(workflow, triggering_actor="collaborator")[
                    "use_self_hosted_linux"], "false")

    def test_disabled_linux_switch_only_affects_linux(self):
        route = self._route("main", linux_enabled="false")
        self.assertEqual(route["use_self_hosted_linux"], "false")
        self.assertEqual(route["use_self_hosted_macos"], "true")
        self.assertEqual(self._route("coverage", linux_enabled="false")[
            "use_self_hosted_linux"], "false")

    def test_change_size_does_not_override_trusted_runner_routing(self):
        """A trusted run follows the runner gate regardless of changed-file count."""
        self.assertNotIn("SELF_HOSTED_MAX_CHANGED_FILES", self.text)
        self.assertNotIn("outputs.large_change", self.text)
        self.assertNotIn("changed_file_count", self.text)
        self.assertNotIn("SELF_HOSTED_MAX_CHANGED_FILES", self.coverage_text)
        self.assertNotIn("outputs.large_change", self.coverage_text)
        self.assertNotIn("changed_file_count", self.coverage_text)

    def test_linux_jobs_are_selected_only_by_the_trusted_runner_gate(self):
        hosted = self._job_body("linux")
        turnstile = self._job_body("linux-self-hosted-turnstile")
        self_hosted = self._job_body("linux-self-hosted")

        self.assertIn("outputs.use_self_hosted_linux != 'true'", hosted)
        self.assertIn("outputs.use_self_hosted_linux == 'true'", turnstile)
        self.assertIn("outputs.use_self_hosted_linux == 'true'", self_hosted)

    def test_macos_jobs_are_selected_only_by_the_trusted_runner_gate(self):
        hosted = self._job_body("macos")
        self_hosted = self._job_body("macos-self-hosted")

        self.assertIn("outputs.use_self_hosted_macos != 'true'", hosted)
        self.assertIn("outputs.use_self_hosted_macos == 'true'", self_hosted)


if __name__ == "__main__":
    unittest.main()
