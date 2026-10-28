"""Workflow runs are cancelled only when a newer commit supersedes their pull request head.

A cancelled run leaves no verdict for the commit it was testing, so a failure it would have found
surfaces later, somewhere else. That loss is acceptable for exactly one kind of run: a
pull_request run for a commit that is no longer its pull request's head, because the new head's
run replaces it and the old commit can no longer merge. Two GitHub settings cancel runs:

- `cancel-in-progress: true` (or an expression) cancels the running run of the same group.
- Any concurrency group shared between runs also cancels a run that is still queued in that group
  when a newer one arrives, even with `cancel-in-progress: false`.

So every `cancel-in-progress` is either the literal `false` or the head-commit setting below, and
every concurrency group is unique per run, the head-commit group, or listed below with the reason
a shared group is wanted.

The head-commit setting shares one group per workflow and pull request among the events that carry
a new or restored head commit (opened, synchronize, reopened), and cancels only for them. Every
other run gets a group keyed on its own run ID and `cancel-in-progress` false: pushes, including
main, schedules, dispatches, releases, label and edit events, and main.yml called from
pr_base_change.yml for an `edited` event (a called workflow sees its caller's event). An edit
therefore never cancels a current-head run. tools/ci_pull_request_events_tests.py evaluates the
expressions for each of those events.

A group counts as unique only when it contains `github.run_id` and no `&&` or `||`: a conditional
group can be unique for some events and shared for others, so it must be listed.
Allowlist entries that no longer match fail too, so the lists only shrink.
"""

from pathlib import Path
import re
import unittest

from python.runfiles import runfiles

# Pull request activity types that carry a new or restored head commit.
HEAD_COMMIT_ACTIONS = ("opened", "synchronize", "reopened")

_IS_HEAD_COMMIT_EVENT = "github.event_name == 'pull_request' && (%s)" % " || ".join(
    "github.event.action == '%s'" % action for action in HEAD_COMMIT_ACTIONS)

# The only cancelling concurrency setting, character for character, at workflow level.
HEAD_COMMIT_GROUP = (
    "${{ %s && format('ci-{0}-pr-{1}', github.workflow, github.event.pull_request.number) || "
    "format('ci-{0}-{1}', github.workflow, github.run_id) }}" % _IS_HEAD_COMMIT_EVENT)
HEAD_COMMIT_CANCEL = "${{ %s }}" % _IS_HEAD_COMMIT_EVENT

# Pull request CI workflows that cancel the runs of a superseded head, by file name.
SUPERSEDED_HEAD_WORKFLOWS = {
    "bcr_preflight.yml",
    "cmake.yml",
    "editor_wasm.yml",
    "lint.yml",
    "main.yml",
    "sanitizers-pr.yml",
}

# Triggers whose runs must never share the head-commit group: publication and privileged runs.
_FORBIDDEN_TRIGGERS = ("release", "pull_request_target", "workflow_run")

# Concurrency groups shared between runs, keyed by (file name, group expression).
SHARED_GROUPS = {
    ("deploy_docs.yaml", "pages-publish"):
        "Serializes manual and automatic publication of the one complete Pages site; the latest "
        "request supersedes a queued older one while the active transaction remains uncancelled.",
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


def _triggers(workflow):
    """The `on:` block, up to the next top-level key."""
    block = ("\n" + workflow).split("\non:\n", 1)[1]
    return re.split(r"\n(?=\S)", block, maxsplit=1)[0] + "\n"


def _pull_request_types(triggers):
    """The pull_request `types` list, the default list when unset, or None without the trigger."""
    match = re.search(r"(?m)^  pull_request:\n((?:(?:    .*)?\n)*)", triggers)
    if not match:
        return None
    types = re.search(r"(?m)^    types:\s*\[(.*)\]\s*$", match.group(1))
    if not types:
        return list(HEAD_COMMIT_ACTIONS)
    return [value.strip() for value in types.group(1).split(",")]


class ConcurrencyPolicyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        workflows = Path(runfiles.Create().Rlocation("donner/.github/workflows/main.yml")).parent
        cls.workflows = {path.name: path.read_text(encoding="utf-8")
                         for path in sorted(workflows.iterdir())
                         if path.suffix in (".yml", ".yaml")}
        assert len(cls.workflows) > 10, sorted(cls.workflows)

    def test_only_superseded_head_workflows_cancel_runs(self):
        cancelling = set()
        for name, workflow in self.workflows.items():
            for _, cancel in _settings(workflow):
                if cancel not in (None, "false"):
                    cancelling.add(name)
        self.assertEqual(cancelling, SUPERSEDED_HEAD_WORKFLOWS)

    def test_superseded_head_workflows_use_exactly_the_head_commit_setting(self):
        for name in sorted(SUPERSEDED_HEAD_WORKFLOWS):
            with self.subTest(workflow=name):
                workflow = self.workflows[name]
                self.assertEqual(list(_settings(workflow)),
                                 [(HEAD_COMMIT_GROUP, HEAD_COMMIT_CANCEL)])
                self.assertIn("\nconcurrency:\n  group: ", workflow,
                              "the setting must be workflow-level, not job-level")

    def test_superseded_head_workflows_run_pull_requests_only_on_head_commits(self):
        # A label, edit or other activity type would join the per-PR group as a non-cancelling
        # run, where a newer head commit's run would still cancel it while it is queued.
        for name in sorted(SUPERSEDED_HEAD_WORKFLOWS):
            with self.subTest(workflow=name):
                triggers = _triggers(self.workflows[name])
                types = _pull_request_types(triggers)
                self.assertIsNotNone(types, "a superseded-head workflow runs on pull_request")
                self.assertLessEqual(set(types), set(HEAD_COMMIT_ACTIONS))
                for trigger in _FORBIDDEN_TRIGGERS:
                    self.assertNotRegex(triggers, r"(?m)^  %s:" % trigger)

    def test_no_concurrency_group_is_shared_without_a_reason(self):
        shared = set()
        for name, workflow in self.workflows.items():
            for group, _ in _settings(workflow):
                self.assertTrue(group, f"{name}: a concurrency setting without a group")
                if name in SUPERSEDED_HEAD_WORKFLOWS and group == HEAD_COMMIT_GROUP:
                    continue
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

    def test_trigger_parser_reads_default_and_explicit_types(self):
        self.assertEqual(_pull_request_types(_triggers("on:\n  pull_request:\n  push:\n")),
                         list(HEAD_COMMIT_ACTIONS))
        self.assertEqual(_pull_request_types(_triggers(
            "on:\n  pull_request:\n    paths:\n      - a\n\n    types: [edited, labeled]\n"
            "jobs:\n  x:\n    types: [opened]\n")), ["edited", "labeled"])
        self.assertIsNone(_pull_request_types(_triggers("on:\n  push:\njobs:\n")))
        self.assertRegex(_triggers("name: x\non:\n  release:\njobs:\n"), r"(?m)^  release:")

    def test_a_conditional_group_is_not_unique_per_run(self):
        self.assertTrue(_unique_per_run("run-${{ github.workflow }}-${{ github.run_id }}"))
        self.assertFalse(_unique_per_run(HEAD_COMMIT_GROUP))
        self.assertFalse(_unique_per_run("pages-publish"))

if __name__ == "__main__":
    unittest.main()
