"""Pins how pull request edits and base-branch changes reach CI.

Editing a pull request's title or description sends an `edited` event; so does changing its
base branch. CI must not restart, or cancel the run in progress, for an edit that changes no
code, but it must run in full against a new base. GitHub shows a pull request the newest run of
each workflow for its head commit, so an edit run whose jobs are all skipped would also hide
the results of the run it followed. The workflows therefore split the cases:

- main.yml (CI) triggers on opened, synchronize and reopened only.
- pr_base_change.yml triggers on `edited` and calls main.yml when the base changed, or when it
  changed earlier on the same head (so a later edit cannot hide that run's results).
- pr_metadata.yml checks the pull request's text on every event that can change it.

A new head commit (opened, synchronize, reopened) cancels the earlier head's CI runs of the same
pull request; no other event cancels or joins them. ci_concurrency_policy_tests.py requires every
cancelling workflow to carry main.yml's exact setting, so the scenarios below cover them all.
"""

import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from python.runfiles import runfiles

from test_executable_fixtures import ExecutableFixtureDirectory


# Workflows the tests below read by name; every workflow file is scanned for `edited`.
WORKFLOWS = ("main.yml", "pr_base_change.yml", "pr_metadata.yml")


def _all_workflow_names():
    directory = Path(runfiles.Create().Rlocation("donner/.github/workflows/main.yml")).parent
    return sorted(path.name for path in directory.glob("*.y*ml"))


# Workflows that may trigger on `edited`, and why an edit there cannot cancel or restart CI.
EDITED_TRIGGER_ALLOWLIST = {
    "pr_base_change.yml": "Runs only a small detect job for an edit; it calls main.yml (with its "
                          "own concurrency group) only after a base-branch change.",
    "pr_metadata.yml": "Checks the pull request text in a few seconds, in a group of its own.",
}

BASE_CHANGE_RUN_NAME = "Base branch changed"


def _read(name):
    """A workflow's text, or None when the file does not exist."""
    path = Path(runfiles.Create().Rlocation("donner/.github/workflows/%s" % name))
    return path.read_text(encoding="utf-8") if path.is_file() else None


def _block(text, header):
    """The lines indented under `header` (a full line), up to the next line at its level."""
    lines = text.splitlines()
    start = lines.index(header)
    indent = len(header) - len(header.lstrip())
    block = []
    for line in lines[start + 1:]:
        if line.strip() and len(line) - len(line.lstrip()) <= indent:
            break
        block.append(line)
    return block


def _pull_request_types(workflow):
    """The `types` list of the pull_request trigger, the default list when unset, or None."""
    if "\n  pull_request:" not in "\n" + workflow.split("\njobs:", 1)[0]:
        return None
    block = _block(workflow, "  pull_request:")
    for line in block:
        line = re.sub(r"\s+#.*$", "", line)
        if re.match(r"^\s+types:", line):
            match = re.fullmatch(r"    types:\s*\[(.*)\]\s*", line)
            if not match:
                raise ValueError("unsupported pull_request types: %r" % line)
            return [value.strip() for value in match.group(1).split(",")]
    return ["opened", "synchronize", "reopened"]


def _concurrency(workflow):
    """(group, cancel-in-progress) of the workflow-level concurrency block, or None."""
    if "\nconcurrency:\n" not in workflow:
        return None
    block = _block(workflow, "concurrency:")
    values = dict(re.match(r"^  ([a-z-]+):\s*(.+)$", line).groups()
                  for line in block if line.strip() and not line.lstrip().startswith("#"))
    return values["group"], values["cancel-in-progress"]


# --- A small evaluator for the GitHub expressions in concurrency settings. -------------------

_TOKEN = re.compile(r"\s*(?:('(?:[^']|'')*')|(==|!=|&&|\|\||[()!,])|([A-Za-z_][A-Za-z0-9_.-]*))")


def _tokens(expression):
    position = 0
    tokens = []
    while position < len(expression.rstrip()):
        match = _TOKEN.match(expression, position)
        if not match or match.end() == position:
            raise ValueError("cannot read expression at %r" % expression[position:])
        string, operator, name = match.groups()
        if string is not None:
            tokens.append(("str", string[1:-1].replace("''", "'")))
        elif operator is not None:
            tokens.append(("op", operator))
        else:
            tokens.append(("name", name))
        position = match.end()
    return tokens


def _equal(left, right):
    if isinstance(left, str) and isinstance(right, str):
        return left.lower() == right.lower()
    return left == right


class _Evaluator:
    """Evaluates `&&`, `||`, `!`, `==`, `!=`, parentheses and format() over a context dict.

    `&&` and `||` return an operand, as GitHub's do. A context name missing from the dict
    raises, so a scenario must state every value the expression reads.
    """

    def __init__(self, expression, context):
        self.tokens = _tokens(expression)
        self.index = 0
        self.context = context

    def evaluate(self):
        value = self._or()
        if self.index != len(self.tokens):
            raise ValueError("trailing tokens: %r" % self.tokens[self.index:])
        return value

    def _peek(self, value):
        return self.index < len(self.tokens) and self.tokens[self.index] == ("op", value)

    def _take(self, value):
        if not self._peek(value):
            raise ValueError("expected %r at %r" % (value, self.tokens[self.index:]))
        self.index += 1

    def _or(self):
        value = self._and()
        while self._peek("||"):
            self.index += 1
            right = self._and()
            value = value if value else right
        return value

    def _and(self):
        value = self._comparison()
        while self._peek("&&"):
            self.index += 1
            right = self._comparison()
            value = right if value else value
        return value

    def _comparison(self):
        value = self._unary()
        for operator in ("==", "!="):
            if self._peek(operator):
                self.index += 1
                equal = _equal(value, self._unary())
                return equal if operator == "==" else not equal
        return value

    def _unary(self):
        if self._peek("!"):
            self.index += 1
            return not self._unary()
        return self._primary()

    def _primary(self):
        kind, value = self.tokens[self.index]
        self.index += 1
        if kind == "str":
            return value
        if (kind, value) == ("op", "("):
            inner = self._or()
            self._take(")")
            return inner
        if kind == "name" and value in ("true", "false"):
            return value == "true"
        if kind == "name" and self._peek("("):
            self.index += 1
            arguments = [self._or()]
            while self._peek(","):
                self.index += 1
                arguments.append(self._or())
            self._take(")")
            if value != "format":
                raise ValueError("unsupported function %s" % value)
            return re.sub(r"\{(\d+)\}", lambda match: str(arguments[1 + int(match.group(1))]),
                          arguments[0])
        if kind == "name":
            if value not in self.context:
                raise KeyError("scenario does not set %s" % value)
            return self.context[value]
        raise ValueError("unexpected token %r" % ((kind, value),))


def evaluate(expression, context):
    """Evaluate a `${{ }}` expression, or return a literal value unchanged."""
    match = re.fullmatch(r"\$\{\{(.*)\}\}", expression.strip(), re.DOTALL)
    if not match:
        return {"true": True, "false": False}.get(expression.strip(), expression.strip())
    return _Evaluator(match.group(1), context).evaluate()


def _event(event_name, ref, workflow="CI", action="", number=7, run_id=1001):
    return {
        "github.event_name": event_name,
        "github.ref": ref,
        "github.workflow": workflow,
        "github.run_id": run_id,
        "github.event.action": action,
        "github.event.pull_request.number": number if event_name == "pull_request" else None,
    }


# Runs of main.yml. A base-change run is main.yml called from pr_base_change.yml, which shares
# the caller's github context: its workflow name and its `edited` event. The group must not
# depend on that name alone, so the base-change scenarios are checked under both names.
MAIN_RUNS = {
    "pr-opened": _event("pull_request", "refs/pull/7/merge", action="opened"),
    "pr-push": _event("pull_request", "refs/pull/7/merge", action="synchronize", run_id=1002),
    "pr-reopened": _event("pull_request", "refs/pull/7/merge", action="reopened", run_id=1003),
    "base-change": _event("pull_request", "refs/pull/7/merge", workflow="CI (base change)",
                          action="edited", run_id=1004),
    "base-change-again": _event("pull_request", "refs/pull/7/merge",
                                workflow="CI (base change)", action="edited", run_id=1005),
    "base-change-callee-name": _event("pull_request", "refs/pull/7/merge", action="edited",
                                      run_id=1010),
    "main-push": _event("push", "refs/heads/main", run_id=1006),
    "main-push-next": _event("push", "refs/heads/main", run_id=1007),
    "branch-push": _event("push", "refs/heads/feature", run_id=1008),
    "branch-push-next": _event("push", "refs/heads/feature", run_id=1009),
    "other-pr-push": _event("pull_request", "refs/pull/8/merge", action="synchronize",
                            number=8, run_id=1011),
    "pr-labeled": _event("pull_request", "refs/pull/7/merge", action="labeled", run_id=1012),
    "schedule": _event("schedule", "refs/heads/main", run_id=1013),
    "dispatch": _event("workflow_dispatch", "refs/heads/main", run_id=1014),
    "release": _event("release", "refs/tags/v1.0.0", run_id=1015),
}


def _detect_script(workflow):
    """The detect job's shell script, dedented as GitHub runs it."""
    job = workflow.split("\n  detect:\n", 1)[1].split("\n  base-change-ci:\n", 1)[0]
    lines = job.splitlines()
    start = next(index for index, line in enumerate(lines) if line.strip() == "run: |")
    indent = len(lines[start]) - len(lines[start].lstrip()) + 2
    script = []
    for line in lines[start + 1:]:
        if line.strip() and len(line) - len(line.lstrip()) < indent:
            break
        script.append(line[indent:])
    return "\n".join(script) + "\n"


def _detect_env(workflow):
    job = workflow.split("\n  detect:\n", 1)[1].split("\n  base-change-ci:\n", 1)[0]
    return dict(re.findall(r"(?m)^          ([A-Z_]+): (.+)$", job))


class CiPullRequestEventsTest(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls):
        cls.workflows = {name: _read(name) for name in WORKFLOWS}
        cls.executables = ExecutableFixtureDirectory()
        cls.addClassCleanup(cls.executables.cleanup)
        cls.executables.write_python3()
        # Records its arguments, then prints STUB_GH_OUTPUT and exits with STUB_GH_EXIT.
        cls.executables.write(
            "gh", '#!/bin/sh\nprintf "%s\\n" "$@" > "$STUB_GH_ARGS"\n'
                  'printf "%s" "$STUB_GH_OUTPUT"\nexit "${STUB_GH_EXIT:-0}"\n')

    def _workflow(self, name):
        workflow = self.workflows[name]
        if workflow is None:
            self.fail("%s does not exist" % name)
        return workflow

    # --- Triggers --------------------------------------------------------------------------

    def test_ci_runs_on_pushes_to_a_pull_request_but_not_on_edits(self):
        self.assertEqual(_pull_request_types(self._workflow("main.yml")),
                         ["opened", "synchronize", "reopened"])

    def test_only_reviewed_workflows_trigger_on_pull_request_edits(self):
        names = _all_workflow_names()
        self.assertIn("main.yml", names)
        edited = {name for name in names
                  if "edited" in (_pull_request_types(_read(name)) or [])}
        self.assertEqual(sorted(edited), sorted(EDITED_TRIGGER_ALLOWLIST))

    def test_metadata_checks_run_on_every_event_that_changes_the_text(self):
        workflow = self._workflow("pr_metadata.yml")
        self.assertEqual(_pull_request_types(workflow),
                         ["opened", "synchronize", "reopened", "edited"])
        self.assertIn('run: python3 tools/check_pr_body.py "$GITHUB_EVENT_PATH"', workflow)
        self.assertNotIn("check_pr_body", self._workflow("main.yml"))

    def test_base_change_workflow_triggers_only_on_edits_and_calls_ci(self):
        workflow = self._workflow("pr_base_change.yml")
        self.assertEqual(_pull_request_types(workflow), ["edited"])
        self.assertNotRegex(workflow, r"(?m)^  (?:push|schedule|workflow_dispatch):")
        call = workflow.split("\n  base-change-ci:\n", 1)[1]
        self.assertIn("    needs: detect\n", call)
        self.assertIn("    if: ${{ needs.detect.outputs.run_ci == 'true' }}\n", call)
        self.assertIn("    uses: ./.github/workflows/main.yml\n", call)
        self.assertRegex(self._workflow("main.yml"), r"(?m)^  workflow_call:$")

    # --- Concurrency -----------------------------------------------------------------------

    def _main_concurrency(self, run):
        group, cancel = _concurrency(self._workflow("main.yml"))
        context = MAIN_RUNS[run]
        return evaluate(group, context), evaluate(cancel, context) is True

    def test_metadata_and_base_change_runs_never_share_a_group_with_ci(self):
        group, cancel = _concurrency(self._workflow("pr_metadata.yml"))
        self.assertIn("github.run_id", group)
        self.assertEqual(cancel, "false")
        # The caller sets no group: the called main.yml's group governs, and an identical
        # caller group would deadlock the call.
        self.assertIsNone(_concurrency(self._workflow("pr_base_change.yml")))

    def test_a_new_head_commit_cancels_the_earlier_heads_runs(self):
        for run in ("pr-opened", "pr-push", "pr-reopened"):
            with self.subTest(run=run):
                group, cancel = self._main_concurrency(run)
                self.assertEqual(group, "ci-CI-pr-7")
                self.assertTrue(cancel)
        other_group, _ = self._main_concurrency("other-pr-push")
        self.assertEqual(other_group, "ci-CI-pr-8")

    def test_a_base_change_run_neither_cancels_nor_joins_other_runs(self):
        push_group, _ = self._main_concurrency("pr-push")
        base_group, cancel = self._main_concurrency("base-change")
        self.assertNotEqual(base_group, push_group)
        self.assertFalse(cancel)
        callee_group, callee_cancel = self._main_concurrency("base-change-callee-name")
        self.assertNotEqual(callee_group, push_group)
        self.assertFalse(callee_cancel)
        again_group, again_cancel = self._main_concurrency("base-change-again")
        self.assertNotEqual(again_group, base_group)
        self.assertFalse(again_cancel)

    def test_every_other_run_keeps_its_own_never_cancelled_group(self):
        head_group, _ = self._main_concurrency("pr-push")
        groups = {head_group}
        for run in ("main-push", "main-push-next", "branch-push", "branch-push-next",
                    "pr-labeled", "schedule", "dispatch", "release"):
            with self.subTest(run=run):
                group, cancel = self._main_concurrency(run)
                self.assertFalse(cancel)
                self.assertNotIn(group, groups)
                groups.add(group)

    def test_evaluator_follows_github_operand_semantics(self):
        context = {"a": "", "b": "x"}
        self.assertEqual(evaluate("${{ a && 'y' || 'z' }}", context), "z")
        self.assertEqual(evaluate("${{ b && 'y' || 'z' }}", context), "y")
        self.assertTrue(evaluate("${{ 'Main' == 'main' }}", context))
        self.assertEqual(evaluate("${{ format('p-{0}-{1}', b, 'q') }}", context), "p-x-q")
        with self.assertRaises(KeyError):
            evaluate("${{ unknown == 'x' }}", context)

    # --- Base-change detection -------------------------------------------------------------

    def _detect(self, base_from="", runs=None, gh_exit=0, gh_output=None):
        workflow = self._workflow("pr_base_change.yml")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            output.touch()
            arguments = Path(directory) / "gh-args"
            if gh_output is None:
                gh_output = json.dumps([{"workflow_runs": runs or []}])
            environment = {
                "PATH": "%s:%s" % (self.executables.path, os.environ.get("PATH", "/usr/bin:/bin")),
                "GITHUB_OUTPUT": str(output),
                "BASE_FROM": base_from,
                "HEAD_SHA": "0123abc",
                "REPOSITORY": "owner/repo",
                "RUN_ID": "500",
                "BASE_CHANGE_RUN_NAME": _detect_env(workflow)["BASE_CHANGE_RUN_NAME"],
                "STUB_GH_ARGS": str(arguments),
                "STUB_GH_OUTPUT": gh_output,
                "STUB_GH_EXIT": str(gh_exit),
            }
            result = subprocess.run(["/bin/bash", "-c", _detect_script(workflow)],
                                    env=environment, capture_output=True, text=True,
                                    check=False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            called = arguments.read_text(encoding="utf-8").splitlines() \
                if arguments.exists() else None
            return output.read_text(encoding="utf-8").strip(), called

    def test_a_base_change_runs_full_ci_without_asking_the_api(self):
        self.assertEqual(self._detect(base_from="main"), ("run_ci=true", None))

    def test_a_title_or_description_edit_skips_ci(self):
        other = {"id": 400, "display_title": "Pull request edited"}
        itself = {"id": 500, "display_title": BASE_CHANGE_RUN_NAME}
        output, called = self._detect(runs=[other, itself])
        self.assertEqual(output, "run_ci=false")
        self.assertIn("repos/owner/repo/actions/workflows/pr_base_change.yml/runs", called)
        self.assertIn("head_sha=0123abc", called)
        self.assertIn("event=pull_request", called)

    def test_an_edit_after_a_base_change_on_the_same_head_reruns_ci(self):
        earlier = {"id": 300, "display_title": BASE_CHANGE_RUN_NAME}
        self.assertEqual(self._detect(runs=[earlier])[0], "run_ci=true")
        pages = json.dumps([{"workflow_runs": []}, {"workflow_runs": [earlier]}])
        self.assertEqual(self._detect(gh_output=pages)[0], "run_ci=true")

    def test_a_later_base_change_does_not_count_as_earlier(self):
        # An edit whose detect job runs after a later retarget's run exists must not rerun CI
        # (it would cancel the retarget's run while testing the old base).
        later = {"id": 600, "display_title": BASE_CHANGE_RUN_NAME}
        self.assertEqual(self._detect(runs=[later])[0], "run_ci=false")

    def test_detection_fails_toward_running_ci(self):
        self.assertEqual(self._detect(gh_exit=1)[0], "run_ci=true")
        self.assertEqual(self._detect(gh_output="not json")[0], "run_ci=true")

    def test_base_change_runs_are_named_for_detection(self):
        workflow = self._workflow("pr_base_change.yml")
        self.assertIn("run-name: ${{ github.event.changes.base && '%s' || "
                      "'Pull request edited' }}\n" % BASE_CHANGE_RUN_NAME, workflow)
        self.assertEqual(_detect_env(workflow)["BASE_CHANGE_RUN_NAME"], BASE_CHANGE_RUN_NAME)
        self.assertEqual(_detect_env(workflow)["BASE_FROM"],
                         "${{ github.event.changes.base.ref.from }}")
        for name, expected in (("pull request edited", False), ("base change", True)):
            with self.subTest(event=name):
                changes = {"github.event.changes.base": {"ref": {"from": "main"}}} \
                    if expected else {"github.event.changes.base": None}
                title = evaluate(re.search(r"(?m)^run-name: (.+)$", workflow).group(1), changes)
                self.assertEqual(title == BASE_CHANGE_RUN_NAME, expected)


if __name__ == "__main__":
    unittest.main()
