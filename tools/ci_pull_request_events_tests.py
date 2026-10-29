"""Pins how pull request edits and base-branch changes reach CI.

Editing a pull request's title or description sends an `edited` event; so does changing its
base branch. CI must not restart, or cancel the run in progress, for an edit that changes no
code, but it must run in full against a new base. The workflows therefore split the cases:

- main.yml (CI) triggers on opened, synchronize and reopened only.
- pr_base_change.yml triggers on `edited` and calls main.yml only when that event changed the
  base. The decision reads the event alone: no API call, so an API outage or rate limit cannot
  start CI. A pull request keeps every run's checks on its head, so a later edit run, whose call
  job is skipped, does not hide an earlier base-change run's results and need not repeat them.
- pr_metadata.yml checks the pull request's text on every event that can change it.

A new head commit (opened, synchronize, reopened) cancels the earlier head's CI runs of the same
pull request; no other event cancels or joins them. ci_concurrency_policy_tests.py requires every
cancelling workflow to carry main.yml's exact setting, so the scenarios below cover them all.
"""

from pathlib import Path
import re
import unittest

from python.runfiles import runfiles


# Workflows the tests below read by name; every workflow file is scanned for `edited`.
WORKFLOWS = ("main.yml", "pr_base_change.yml", "pr_metadata.yml")


def _all_workflow_names():
    directory = Path(runfiles.Create().Rlocation("donner/.github/workflows/main.yml")).parent
    return sorted(path.name for path in directory.glob("*.y*ml"))


# Workflows that may trigger on `edited`, and why an edit there cannot cancel or restart CI.
EDITED_TRIGGER_ALLOWLIST = {
    "pr_base_change.yml": "Starts no job for a title or description edit; it calls main.yml (with "
                          "its own concurrency group) only for a base-branch change.",
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


def _code(workflow):
    """The workflow without comment lines, so prose cannot satisfy or fail a marker check."""
    return "\n".join(line for line in workflow.splitlines()
                     if not line.lstrip().startswith("#"))


def _base_change_job(workflow):
    """The lines of the base-change-ci job, the workflow's only job."""
    return _block(workflow, "  base-change-ci:")


def _base_change_condition(workflow):
    """The base-change-ci job's `if` expression."""
    for line in _base_change_job(workflow):
        match = re.fullmatch(r"    if: (.+)", line)
        if match:
            return match.group(1)
    raise AssertionError("base-change-ci has no if condition")


# The `changes` object of `edited` events. GitHub sets `changes.base` only when the base branch
# changed; a title or description edit carries only `title` or `body`.
EDITED_EVENTS = {
    "title edit": {"title": {"from": "Old title"}},
    "description edit": {"body": {"from": "Old description"}},
    "title and description edit": {"title": {"from": "Old"}, "body": {"from": "Old"}},
    "base change": {"base": {"ref": {"from": "main"}, "sha": {"from": "0123abc"}}},
    "base change with a title edit": {"title": {"from": "Old"},
                                      "base": {"ref": {"from": "parent"},
                                               "sha": {"from": "0123abc"}}},
}


class CiPullRequestEventsTest(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls):
        cls.workflows = {name: _read(name) for name in WORKFLOWS}

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
        call = _base_change_job(workflow)
        self.assertIn("    uses: ./.github/workflows/main.yml", call)
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

    # --- Base-change decision --------------------------------------------------------------

    def _runs_ci(self, changes):
        """Whether an `edited` event with these `changes` starts the main.yml call.

        The evaluator raises for any context name the scenario does not set, so the condition
        may read nothing but the event's own `changes.base`.
        """
        condition = _base_change_condition(self._workflow("pr_base_change.yml"))
        value = evaluate(condition, {"github.event.changes.base": changes.get("base")})
        # GitHub treats every object as true, even an empty one; Python's bool() does not.
        return True if isinstance(value, dict) else bool(value)

    def test_a_title_or_description_edit_runs_no_ci(self):
        for name in ("title edit", "description edit", "title and description edit"):
            with self.subTest(event=name):
                self.assertFalse(self._runs_ci(EDITED_EVENTS[name]))

    def test_a_base_change_runs_full_ci(self):
        for name in ("base change", "base change with a title edit"):
            with self.subTest(event=name):
                self.assertTrue(self._runs_ci(EDITED_EVENTS[name]))

    def test_the_call_is_the_only_job(self):
        # A skipped call is the whole of an edit run: no detect job or other step takes a
        # runner, and nothing else can start main.yml.
        workflow = self._workflow("pr_base_change.yml")
        jobs = re.findall(r"(?m)^  ([A-Za-z0-9_-]+):$", workflow.split("\njobs:\n", 1)[1])
        self.assertEqual(jobs, ["base-change-ci"])
        self.assertNotRegex(workflow, r"(?m)^\s+(?:needs|steps|runs-on):")

    def test_an_earlier_base_change_on_the_head_is_not_rerun(self):
        # GitHub keeps the checks of every run on a pull request's head, so the base-change
        # run stays visible after later edit runs. The decision therefore never looks up the
        # head's earlier runs: an edit after a base change is an ordinary edit.
        workflow = _code(self._workflow("pr_base_change.yml"))
        for marker in ("actions/workflows", "head_sha", "github.run_id", "display_title"):
            with self.subTest(marker=marker):
                self.assertNotIn(marker, workflow)
        self.assertFalse(self._runs_ci(EDITED_EVENTS["description edit"]))

    def test_an_api_failure_cannot_start_ci(self):
        # The previous design listed earlier runs through the API and ran full CI when that
        # call failed, so a rate limit turned description edits into full CI runs. The
        # decision now makes no API call and holds no token that could make one.
        workflow = _code(self._workflow("pr_base_change.yml"))
        for marker in ("gh api", "GH_TOKEN", "github.token", "curl", "run:"):
            with self.subTest(marker=marker):
                self.assertNotIn(marker, workflow)

    def test_base_change_runs_are_named(self):
        workflow = self._workflow("pr_base_change.yml")
        self.assertIn("run-name: ${{ github.event.changes.base && '%s' || "
                      "'Pull request edited' }}\n" % BASE_CHANGE_RUN_NAME, workflow)
        run_name = re.search(r"(?m)^run-name: (.+)$", workflow).group(1)
        for name, changes in EDITED_EVENTS.items():
            with self.subTest(event=name):
                title = evaluate(run_name, {"github.event.changes.base": changes.get("base")})
                self.assertEqual(title == BASE_CHANGE_RUN_NAME, "base" in changes)


if __name__ == "__main__":
    unittest.main()
