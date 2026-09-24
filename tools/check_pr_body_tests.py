"""Regression tests for the public pull request body gate."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from python.runfiles import runfiles

from check_pr_body import has_tool_session_link


class CheckPrBodyTest(unittest.TestCase):
    def test_rejects_claude_session_link(self):
        self.assertTrue(has_tool_session_link("See https://claude.ai/code/session_example123"))

    def test_rejects_codex_task_link_inside_markdown(self):
        body = "[Task](https://chatgpt.com/codex/tasks/example123)"
        self.assertTrue(has_tool_session_link(body))

    def test_accepts_public_product_and_project_links(self):
        body = "Docs: https://platform.openai.com/docs and https://github.com/example/project/pull/1"
        self.assertFalse(has_tool_session_link(body))

    def test_cli_rejects_link_without_echoing_it(self):
        link = "https://claude.ai/code/session_example123"
        with tempfile.TemporaryDirectory() as directory:
            event = Path(directory) / "event.json"
            event.write_text(json.dumps({"pull_request": {"body": f"Evidence: {link}"}}))
            result = subprocess.run(
                [sys.executable, str(Path(__file__).with_name("check_pr_body.py")), str(event)],
                capture_output=True,
                text=True,
                check=False,
            )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertNotIn(link, result.stdout + result.stderr)

    def test_cli_accepts_a_body_without_session_links(self):
        with tempfile.TemporaryDirectory() as directory:
            event = Path(directory) / "event.json"
            event.write_text(json.dumps({"pull_request": {"body": "Fixes SVG handling."}}))
            result = subprocess.run(
                [sys.executable, str(Path(__file__).with_name("check_pr_body.py")), str(event)],
                capture_output=True,
                text=True,
                check=False,
            )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_cli_fails_closed_without_pull_request_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            event = Path(directory) / "event.json"
            event.write_text("{}")
            result = subprocess.run(
                [sys.executable, str(Path(__file__).with_name("check_pr_body.py")), str(event)],
                capture_output=True,
                text=True,
                check=False,
            )
        self.assertNotEqual(result.returncode, 0)

    def test_gatekeeper_runs_on_edited_pull_request_bodies(self):
        source = Path(runfiles.Create().Rlocation("donner/.github/workflows/main.yml")).read_text()
        self.assertTrue(
            "types: [opened, synchronize, reopened, edited]" in source,
            "CI must rerun the PR body gate when the body is edited",
        )
        gatekeeper = source.split("  gatekeeper:\n", 1)[1].split("\n  determine-targets:", 1)[0]
        self.assertTrue(
            "python3 tools/check_pr_body.py" in gatekeeper,
            "Gatekeeper must run the PR-body check",
        )


if __name__ == "__main__":
    unittest.main()
