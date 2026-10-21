"""Workflow runs are never cancelled to make room for newer ones.

A cancelled run leaves no verdict for the commit it was testing, so a failure it would have found
surfaces later, somewhere else. Two GitHub settings cancel runs:

- `cancel-in-progress: true` (or an expression) cancels the running run of the same group.
- Any concurrency group shared between runs also cancels a run that is still queued in that group
  when a newer one arrives, even with `cancel-in-progress: false`.

So every `cancel-in-progress` is the literal `false`, and every concurrency group is either unique
per run or listed below with the reason a shared group is wanted. A group counts as unique only
when it contains `github.run_id` and no `&&` or `||`: a conditional group can be unique for some
events and shared for others, so it must be listed.
Allowlist entries that no longer match fail too, so the lists only shrink.
"""

from pathlib import Path
import re
import unittest

from python.runfiles import runfiles

# Workflows that still cancel a running run, by file name.
CANCELLING_WORKFLOWS = {}

# Concurrency groups shared between runs, keyed by (file name, group expression).
SHARED_GROUPS = {
    ("deploy_docs.yaml", "pages-publish"):
        "Serializes manually dispatched promotions of the one Pages site; the latest dispatch "
        "supersedes a queued older one.",
    ("publish_bcr.yml", "bcr-publication"):
        "Serializes manual BCR submissions, which push to one fork; a queued dispatch that a "
        "third one replaces is re-run by hand.",
    ("release.yml", "release-${{ github.event.release.tag_name }}"):
        "One publication per release tag.",
    ("rerun_runner_loss.yml",
     "rerun-runner-loss-${{ github.event.workflow_run.id || inputs.run_id }}"):
        "At most one rerun decision per CI run it inspects.",
}

# Workflow-level (column 0) and job-level (column 4) settings. Deeper lines, such as a script line
# inside a `run: |` block, are not settings.
_CONCURRENCY = re.compile(r"^(?P<indent>(?:    )?)concurrency:(?P<inline>[^\n]*)\n", re.MULTILINE)


def _scalar(value):
    """A plain YAML scalar without a trailing comment or surrounding quotes."""
    value = re.sub(r"\s+#.*$", "", value).strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
        value = value[1:-1]
    return value


def _settings(workflow):
    """Yields (group, cancel-in-progress or None) for each concurrency setting in `workflow`."""
    for match in _CONCURRENCY.finditer(workflow):
        inline = _scalar(match.group("inline"))
        if inline.startswith("{"):
            raise ValueError("flow-mapping concurrency settings are not supported: " + inline)
        if inline:
            yield inline, None
            continue
        values = {}
        indent = len(match.group("indent"))
        for line in workflow[match.end():].splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            if len(line) - len(line.lstrip(" ")) <= indent:
                break
            key, _, value = stripped.partition(":")
            values[key] = _scalar(value)
        yield values.get("group"), values.get("cancel-in-progress")


def _unique_per_run(group):
    return "github.run_id" in group and "&&" not in group and "||" not in group


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
                if not _unique_per_run(group) or key in SHARED_GROUPS:
                    shared.add(key)
        self.assertEqual(shared, set(SHARED_GROUPS))

    def test_parser_reads_block_inline_and_job_level_settings(self):
        workflow = ("concurrency:\n  # Why.\n  group: a-${{ github.run_id }}\n\n"
                    "  cancel-in-progress: 'false'  # trailing\n"
                    "jobs:\n  deploy:\n    concurrency:\n      group: pages\n"
                    "      cancel-in-progress: ${{ x }}\n    steps:\n"
                    "      - run: |\n          concurrency: not-a-setting\n"
                    "  other:\n    concurrency: 'inline-group'  # note\n")
        self.assertEqual(list(_settings(workflow)), [
            ("a-${{ github.run_id }}", "false"),
            ("pages", "${{ x }}"),
            ("inline-group", None),
        ])
        with self.assertRaises(ValueError):
            list(_settings("concurrency: {group: g, cancel-in-progress: true}\n"))

    def test_a_conditional_group_is_not_unique_per_run(self):
        self.assertTrue(_unique_per_run("run-${{ github.workflow }}-${{ github.run_id }}"))
        self.assertFalse(_unique_per_run(
            "${{ github.event_name == 'pull_request' && format('pr-{0}', "
            "github.event.pull_request.number) || format('x-{0}', github.run_id) }}"))
        self.assertFalse(_unique_per_run("pages-publish"))

if __name__ == "__main__":
    unittest.main()
