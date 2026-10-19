"""Workflow runs are never cancelled to make room for newer ones.

A cancelled run leaves no verdict for the commit it was testing, so a failure it would have found
surfaces later, somewhere else. Two GitHub settings cancel runs:

- `cancel-in-progress: true` (or an expression) cancels the running run of the same group.
- Any concurrency group shared between runs also cancels a run that is still queued in that group
  when a newer one arrives, even with `cancel-in-progress: false`.

So every `cancel-in-progress` is the literal `false`, and every concurrency group either contains
`github.run_id` (one run per group) or is listed below with the reason a shared group is wanted.
The check is textual: a group expression that uses `github.run_id` on only some events must be
listed by a reviewer, not inferred.
Allowlist entries that no longer match fail too, so the lists only shrink.
"""

from pathlib import Path
import re
import unittest

from python.runfiles import runfiles

# Workflows that still cancel a running run, by file name.
CANCELLING_WORKFLOWS = {
    "main.yml":
        "Pull request runs are superseded by the next push; its concurrency block changes with "
        "the main CI workflow's own refactor.",
}

# Concurrency groups shared between runs, keyed by (file name, group expression).
SHARED_GROUPS = {
    # "*": any group in that workflow, so other changes to its block do not churn this entry.
    ("main.yml", "*"):
        "Changes with the main CI workflow's own refactor, as above.",
    ("deploy_docs.yaml", "pages-publish"):
        "Serializes deployments of the one Pages site; a queued older deployment is stale.",
    ("publish_bcr.yml", "bcr-publication"):
        "Serializes BCR submissions, which push to one fork.",
    ("release.yml", "release-${{ github.event.release.tag_name }}"):
        "One publication per release tag.",
    ("rerun_runner_loss.yml",
     "rerun-runner-loss-${{ github.event.workflow_run.id || inputs.run_id }}"):
        "At most one rerun decision per CI run it inspects.",
}

_CONCURRENCY = re.compile(r"^(?P<indent> *)concurrency:(?P<inline>[^\n]*)\n", re.MULTILINE)


def _settings(workflow):
    """Yields (group, cancel-in-progress or None) for each concurrency setting in `workflow`."""
    for match in _CONCURRENCY.finditer(workflow):
        inline = match.group("inline").strip()
        if inline:
            yield inline.strip("'\""), None
            continue
        values = {}
        indent = len(match.group("indent"))
        for line in workflow[match.end():].splitlines():
            if line.strip() and len(line) - len(line.lstrip(" ")) <= indent:
                break
            key, _, value = line.strip().partition(":")
            values[key] = value.strip()
        yield values.get("group"), values.get("cancel-in-progress")


class ConcurrencyPolicyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        workflows = Path(runfiles.Create().Rlocation("donner/.github/workflows/main.yml")).parent
        cls.workflows = {path.name: path.read_text(encoding="utf-8")
                         for path in sorted(workflows.iterdir())
                         if path.suffix in (".yml", ".yaml")}
        assert len(cls.workflows) > 10, sorted(cls.workflows)

    def test_no_workflow_cancels_a_running_run(self):
        cancelling = set()
        for name, workflow in self.workflows.items():
            for _, cancel in _settings(workflow):
                if cancel not in (None, "false"):
                    cancelling.add(name)
        self.assertEqual(cancelling, set(CANCELLING_WORKFLOWS))

    def test_no_concurrency_group_is_shared_without_a_reason(self):
        shared = set()
        for name, workflow in self.workflows.items():
            for group, _ in _settings(workflow):
                self.assertTrue(group, f"{name}: a concurrency setting without a group")
                key = (name, "*") if (name, "*") in SHARED_GROUPS else (name, group)
                if "github.run_id" not in group or key in SHARED_GROUPS:
                    shared.add(key)
        self.assertEqual(shared, set(SHARED_GROUPS))

    def test_parser_reads_block_inline_and_job_level_settings(self):
        workflow = ("concurrency:\n  group: a-${{ github.run_id }}\n  cancel-in-progress: false\n"
                    "jobs:\n  deploy:\n    concurrency:\n      group: pages\n"
                    "      cancel-in-progress: ${{ x }}\n    steps: []\n"
                    "  other:\n    concurrency: 'inline-group'\n")
        self.assertEqual(list(_settings(workflow)), [
            ("a-${{ github.run_id }}", "false"),
            ("pages", "${{ x }}"),
            ("inline-group", None),
        ])


if __name__ == "__main__":
    unittest.main()
