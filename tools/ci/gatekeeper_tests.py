"""Runs the gatekeeper decisions as a workflow step would: real git history, a fake gh."""

import json
import os
import shlex
import textwrap
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from python.runfiles import runfiles
from tools import test_timing
from tools.test_executable_fixtures import ExecutableFixtureDirectory


# Host and user git configuration (global ignores, hooks, signing) never reaches the fixtures,
# and git never searches above a fixture's temporary directory for an enclosing repository.
_GIT_ENV = {"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CEILING_DIRECTORIES": tempfile.gettempdir()}


def _git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True, stdout=subprocess.PIPE,
                          env=dict(os.environ, **_GIT_ENV), text=True,
                          timeout=test_timing.STALL_GUARD_SECONDS).stdout.strip()


def _commit(repo, paths, message):
    for path in paths:
        target = Path(repo) / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(message + "\n", encoding="utf-8")
        # Forced: a fixture may itself commit a .gitignore whose content ignores later paths.
        _git(repo, "add", "-f", "--", path)
    _git(repo, "commit", "-q", "-m", message)
    return _git(repo, "rev-parse", "HEAD")


class GatekeeperTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.script = runfiles.Create().Rlocation("donner/tools/ci/gatekeeper.py")
        cls.executables = ExecutableFixtureDirectory()
        cls.addClassCleanup(cls.executables.cleanup)
        fake_gh = cls.executables.path / "fake_gh.py"
        # GH_RATE_LIMITED_CALLS refuses that many leading calls the way GitHub's rate limit
        # does; `gh api rate_limit` reports an exhausted window that resets now.
        fake_gh.write_text(
            "import json, os, sys, time\n"
            "with open(os.environ['GH_CAPTURE'], 'a', encoding='utf-8') as capture:\n"
            "    capture.write(json.dumps(sys.argv[1:]) + '\\n')\n"
            "if sys.argv[1:3] == ['api', 'rate_limit']:\n"
            "    sys.stdout.write(json.dumps({'resources': {'core': {'limit': 1000,\n"
            "        'remaining': 0, 'used': 1000, 'reset': int(time.time())}}}))\n"
            "    sys.exit(0)\n"
            "state = os.environ.get('GH_RATE_LIMIT_STATE')\n"
            "if state:\n"
            "    refused = int(open(state).read() or '0') if os.path.exists(state) else 0\n"
            "    if refused < int(os.environ['GH_RATE_LIMITED_CALLS']):\n"
            "        open(state, 'w').write(str(refused + 1))\n"
            "        sys.stderr.write('HTTP 403: API rate limit exceeded for installation.\\n')\n"
            "        sys.exit(1)\n"
            "sys.stdout.write(os.environ['GH_STDOUT'])\n"
            "sys.exit(int(os.environ['GH_STATUS']))\n",
            encoding="utf-8",
        )
        cls.executables.write_python_entry("gh", fake_gh)
        cls.executables.write_python3()

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="gatekeeper ")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.repo = self.work / "repo"
        self.repo.mkdir()
        _git(self.repo, "init", "-q", "-b", "main")
        _git(self.repo, "config", "user.name", "Fixture")
        _git(self.repo, "config", "user.email", "fixture@example.com")
        self.root = _commit(self.repo, ["README.md"], "root")

    def _run(self, decision, gh_stdout="0\n", gh_status=0, args=(), cwd=None,
             workflow_step=None, **context):
        output = self.work / "github_output"
        output.write_text("", encoding="utf-8")
        capture = self.work / "gh_calls.jsonl"
        capture.write_text("", encoding="utf-8")
        env = dict(os.environ,
                   PATH=f"{self.executables.path}{os.pathsep}{os.environ.get('PATH', '')}",
                   GITHUB_OUTPUT=str(output), GH_CAPTURE=str(capture), GH_STDOUT=gh_stdout,
                   GH_STATUS=str(gh_status), GITHUB_REPOSITORY="owner/repo",
                   GITHUB_REF="refs/heads/feature", GITHUB_REF_NAME="feature", **_GIT_ENV)
        env.update(context)
        workflow_step = workflow_step or {"duplicate-push": "dup_push",
                                           "docs-only": "docs_only"}.get(decision)
        command = (self._workflow_command(workflow_step, env) if workflow_step else
                   [sys.executable, self.script, decision, *args])
        result = subprocess.run(command,
                                cwd=cwd or self.repo, env=env,
                                capture_output=True, text=True, check=False,
                                timeout=test_timing.STALL_GUARD_SECONDS)
        calls = [json.loads(line) for line in capture.read_text().splitlines()]
        return result, output.read_text(encoding="utf-8"), calls

    def _workflow_body(self, step):
        workflow = Path(runfiles.Create().Rlocation(
            "donner/.github/workflows/main.yml")).read_text(encoding="utf-8")
        section = workflow.split(f"      - id: {step}\n", 1)[1].split("\n      - ", 1)[0]
        section = section.split("\n  determine-targets:\n", 1)[0]
        run = section.split("        run: ", 1)[1]
        return textwrap.dedent(run[2:]) if run.startswith("|\n") else run.strip()

    def _workflow_command(self, step, env):
        body = self._workflow_body(step)
        for field, variable in (("event_name", "GITHUB_EVENT_NAME"),
                                ("ref", "GITHUB_REF"), ("ref_name", "GITHUB_REF_NAME"),
                                ("repository", "GITHUB_REPOSITORY")):
            body = body.replace("${{ github." + field + " }}", env[variable])
        for command in ('python3 "$GITHUB_WORKSPACE/tools/ci/gatekeeper.py"',
                        "python3 tools/ci/gatekeeper.py"):
            body = body.replace(command, shlex.join([sys.executable, self.script]))
        self.assertNotRegex(body, r"\$\{\{", "Unmodelled workflow expression")
        return ["env", "-u", "BASH_ENV", "bash", "--noprofile", "--norc",
                "-e", "-o", "pipefail", "-c", body]

    def _pull_request(self, base, head):
        event = self.work / "event.json"
        event.write_text(json.dumps({"pull_request": {"base": {"sha": base},
                                                      "head": {"sha": head}}}),
                         encoding="utf-8")
        return {"GITHUB_EVENT_NAME": "pull_request", "GITHUB_EVENT_PATH": str(event)}

    # duplicate-push

    def test_push_with_open_pull_request_is_duplicate(self):
        result, output, calls = self._run("duplicate-push", gh_stdout="1\n",
                                          GITHUB_EVENT_NAME="push")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, "is_duplicate_push=true\n")
        self.assertEqual(calls, [["pr", "list", "--repo", "owner/repo", "--state", "open",
                                  "--head", "feature", "--json", "number", "-q", "length"]])
        self.assertIn("open PR already covers branch 'feature'", result.stdout)

    def test_main_workflow_treats_branch_shell_syntax_as_data(self):
        branch = "topic-$(touch${IFS}INJECTED)`touch${IFS}INJECTED2`"
        _git(self.repo, "check-ref-format", "--branch", branch)
        result, output, calls = self._run("duplicate-push", gh_stdout="1\n",
                                          workflow_step="dup_push", GITHUB_EVENT_NAME="push",
                                          GITHUB_REF_NAME=branch, GITHUB_REF="refs/heads/" + branch)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([], sorted(path.name for path in self.repo.glob("INJECTED*")),
                         "Branch name executed as shell code")
        self.assertEqual(output, "is_duplicate_push=true\n")
        self.assertEqual(calls, [["pr", "list", "--repo", "owner/repo", "--state", "open",
                                  "--head", branch, "--json", "number", "-q", "length"]])

    def test_comparison_checkout_cannot_replace_admission_or_trust_code(self):
        trusted = self.work / "trusted"
        helper = trusted / "tools/ci/gatekeeper.py"
        action = trusted / ".github/actions/pr-trust/action.yml"
        helper.parent.mkdir(parents=True)
        action.parent.mkdir(parents=True)
        helper.write_bytes(Path(self.script).read_bytes())
        # The admission checkout also holds the rate-limit helper the gatekeeper imports.
        (helper.parent / "github_rate_limit.py").write_bytes(
            (Path(self.script).parent / "github_rate_limit.py").read_bytes())
        action.write_bytes(Path(runfiles.Create().Rlocation(
            "donner/.github/actions/pr-trust/action.yml")).read_bytes())
        originals = (helper.read_bytes(), action.read_bytes())
        self.repo.rename(trusted / "pr-input")
        self.repo = trusted / "pr-input"
        payload = ("import os\nfrom pathlib import Path\nPath('INJECTED').touch()\n"
                   "target = Path(os.environ['GITHUB_WORKSPACE']) / "
                   "'.github/actions/pr-trust/action.yml'\n"
                   "target.write_text('forged trust action')\n")
        _commit(self.repo, ["tools/ci/gatekeeper.py"], payload)
        head = _commit(self.repo, [".github/actions/pr-trust/action.yml"], "forged action")
        context = self._pull_request(self.root, head)
        output = self.work / "admission-output"
        env = dict(os.environ, **context, **_GIT_ENV, GITHUB_WORKSPACE=str(trusted),
                   GITHUB_OUTPUT=str(output), EVENT_NAME="pull_request", ACTOR="contributor",
                   TRIGGERING_ACTOR="contributor", REPOSITORY="jwmcglynn/donner",
                   PATH=f"{self.executables.path}{os.pathsep}{os.environ.get('PATH', '')}")
        self._run_fixture_shell(self._workflow_body("docs_only"), env, self.repo)
        self.assertFalse((self.repo / "INJECTED").exists(), "PR helper executed before trust")
        self.assertEqual(originals, (helper.read_bytes(), action.read_bytes()))
        self.assertEqual("docs_only=false\n", output.read_text())
        trust_body = textwrap.dedent(action.read_text().split("      run: |\n", 1)[1])
        self._run_fixture_shell(trust_body, env, trusted)
        self.assertIn("trusted_pr=false\n", output.read_text())
        env.update(TRUSTED_PR="false", SELFHOSTED_LINUX_RUNNER="true",
                   SELFHOSTED_MACOS_RUNNER="true")
        self._run_fixture_shell(self._workflow_body("runner_gate"), env, trusted)
        self.assertIn("use_self_hosted_linux=false\n", output.read_text())
        self.assertIn("use_self_hosted_macos=false\n", output.read_text())

    def _run_fixture_shell(self, body, env, cwd):
        result = subprocess.run(
            ["env", "-u", "BASH_ENV", "bash", "--noprofile", "--norc", "-e", "-o", "pipefail",
             "-c", body], env=env, cwd=cwd, capture_output=True, text=True, check=False,
            timeout=test_timing.STALL_GUARD_SECONDS)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rate_limited_query_waits_for_the_reset_and_repeats(self):
        result, output, calls = self._run(
            "duplicate-push", gh_stdout="1\n", GITHUB_EVENT_NAME="push",
            GH_RATE_LIMITED_CALLS="1", GH_RATE_LIMIT_STATE=str(self.work / "refused"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, "is_duplicate_push=true\n")
        query = ["pr", "list", "--repo", "owner/repo", "--state", "open", "--head", "feature",
                 "--json", "number", "-q", "length"]
        self.assertEqual(calls, [query, ["api", "rate_limit"], query])
        # The window the refusal reported is logged, so a failure shows when it resets.
        self.assertRegex(result.stderr, r"remaining=0 limit=1000 used=1000 reset=\d{4}-")

    def test_push_without_open_pull_request_runs(self):
        result, output, calls = self._run("duplicate-push", gh_stdout="0\n",
                                          GITHUB_EVENT_NAME="push")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, "is_duplicate_push=false\n")
        self.assertEqual(len(calls), 1)

    def test_main_pushes_and_other_events_never_query_or_skip(self):
        for event, ref in (("push", "refs/heads/main"), ("pull_request", "refs/pull/1/merge"),
                           ("schedule", "refs/heads/main"), ("workflow_dispatch",
                                                             "refs/heads/feature")):
            with self.subTest(event=event, ref=ref):
                result, output, calls = self._run("duplicate-push", gh_stdout="1\n",
                                                  GITHUB_EVENT_NAME=event, GITHUB_REF=ref)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(output, "is_duplicate_push=false\n")
                self.assertEqual(calls, [])

    def test_gh_failure_fails_the_step_without_a_decision(self):
        result, output, _ = self._run("duplicate-push", gh_status=4, GITHUB_EVENT_NAME="push")
        self.assertEqual(result.returncode, 4)
        self.assertEqual(output, "")

    def test_unparseable_count_keeps_the_push_run(self):
        result, output, _ = self._run("duplicate-push", gh_stdout="", GITHUB_EVENT_NAME="push")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, "is_duplicate_push=false\n")

    # docs-only

    def test_docs_only_pull_request(self):
        head = _commit(self.repo, ["docs/guide.md", "CHANGELOG.md", "NOTES.md", ".gitignore",
                                   ".editorconfig", ".gitattributes", "docs/img/a.svg"], "docs")
        result, output, _ = self._run("docs-only", **self._pull_request(self.root, head))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, "docs_only=true\n")
        self.assertIn(f"Using merge-base {self.root} as diff basis.", result.stdout)

    def test_any_build_input_clears_docs_only(self):
        for path in ("donner/BUILD.bazel", "nested/README.md", ".bazelignore", ".gitmodules",
                     "MODULE.bazel", "documentation.txt"):
            with self.subTest(path=path):
                _git(self.repo, "checkout", "-q", "--detach", self.root)
                head = _commit(self.repo, ["docs/guide.md", path], "change " + path)
                result, output, _ = self._run("docs-only", **self._pull_request(self.root, head))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(output, "docs_only=false\n")
                self.assertIn(f"Non-doc change: {path}", result.stdout)

    def test_deletions_are_not_counted(self):
        # Pins the existing ACMR filter: a pull request that only deletes a build input next to
        # documentation edits is classed docs-only. Changing this is a deliberate policy change.
        _commit(self.repo, ["donner/old.cc"], "add source")
        base = _git(self.repo, "rev-parse", "HEAD")
        _git(self.repo, "rm", "-q", "donner/old.cc")
        head = _commit(self.repo, ["docs/guide.md"], "delete source, edit docs")
        result, output, _ = self._run("docs-only", **self._pull_request(base, head))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, "docs_only=true\n")

    def test_base_drift_after_the_fork_is_not_attributed_to_the_pull_request(self):
        head = _commit(self.repo, ["docs/guide.md"], "docs")
        _git(self.repo, "checkout", "-q", "--detach", self.root)
        base_tip = _commit(self.repo, ["donner/code.cc"], "base drift")
        result, output, _ = self._run("docs-only", **self._pull_request(base_tip, head))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, "docs_only=true\n")

    def test_missing_merge_base_falls_back_to_the_base_tip(self):
        _git(self.repo, "checkout", "-q", "--orphan", "unrelated")
        _git(self.repo, "rm", "-q", "-rf", ".")
        unrelated = _commit(self.repo, ["docs/other.md"], "unrelated")
        result, output, _ = self._run("docs-only", **self._pull_request(unrelated, self.root))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"using base tip {unrelated} as diff basis", result.stdout)
        # The fallback diff contains README.md, a top-level Markdown file.
        self.assertEqual(output, "docs_only=true\n")

    def test_empty_change_set_is_not_docs_only(self):
        result, output, _ = self._run("docs-only", **self._pull_request(self.root, self.root))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, "docs_only=false\n")

    def test_non_pull_request_events_are_never_docs_only(self):
        for event in ("push", "schedule", "workflow_dispatch"):
            with self.subTest(event=event):
                result, output, _ = self._run("docs-only", GITHUB_EVENT_NAME=event)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(output, "docs_only=false\n")

    def test_unknown_head_fails_the_step(self):
        result, output, _ = self._run("docs-only", **self._pull_request(self.root, "0" * 40))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output, "")

    # main-advanced

    def _checkout(self):
        """A clone of the fixture repository, which is its origin."""
        checkout = self.work / "checkout"
        subprocess.run(["git", "clone", "-q", str(self.repo), str(checkout)], check=True,
                       env=dict(os.environ, **_GIT_ENV), timeout=test_timing.STALL_GUARD_SECONDS)
        return checkout

    def _advanced(self, gh_stdout, event="schedule", **kwargs):
        return self._run("main-advanced", gh_stdout=gh_stdout, args=("--workflow", "perf.yml"),
                         GITHUB_EVENT_NAME=event, **kwargs)

    def test_manual_dispatch_always_runs_without_queries(self):
        result, output, calls = self._advanced("", event="workflow_dispatch")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, "should_run=true\n")
        self.assertEqual(calls, [])

    def test_unchanged_main_skips(self):
        checkout = self._checkout()
        for event in ("schedule", "push"):
            with self.subTest(event=event):
                result, output, calls = self._advanced(self.root + "\n", event=event,
                                                       cwd=checkout)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(output, "should_run=false\n")
                self.assertEqual(calls, [["run", "list", "--workflow=perf.yml",
                                          "--status=success", "--limit=1", "--json", "headSha",
                                          "--jq", ".[0].headSha"]])
                self.assertIn("has not advanced since the last successful perf.yml run",
                              result.stdout)

    def test_advanced_main_runs(self):
        checkout = self._checkout()
        new_main = _commit(self.repo, ["donner/code.cc"], "advance main")
        result, output, _ = self._advanced(self.root + "\n", cwd=checkout)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, "should_run=true\n")
        self.assertIn(f"advanced from {self.root} to {new_main}", result.stdout)

    def test_no_previous_success_runs(self):
        checkout = self._checkout()
        for stdout in ("", "\n", "null\n"):
            with self.subTest(stdout=stdout):
                result, output, _ = self._advanced(stdout, cwd=checkout)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(output, "should_run=true\n")

    def test_rate_limited_run_list_is_repeated_after_the_reset(self):
        checkout = self._checkout()
        result, output, calls = self._advanced(
            self.root + "\n", cwd=checkout, GH_RATE_LIMITED_CALLS="1",
            GH_RATE_LIMIT_STATE=str(self.work / "refused"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, "should_run=false\n")
        self.assertEqual([call[:2] for call in calls],
                         [["run", "list"], ["api", "rate_limit"], ["run", "list"]])

    def test_query_failures_fail_the_step_without_a_decision(self):
        checkout = self._checkout()
        result, output, _ = self._advanced(self.root, gh_status=4, cwd=checkout)
        self.assertEqual(result.returncode, 4)
        self.assertEqual(output, "")
        # The fixture repository has no origin to fetch main from.
        result, output, calls = self._advanced(self.root)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output, "")
        self.assertEqual(calls, [])

    def test_workflow_is_required(self):
        result, output, calls = self._run("main-advanced", GITHUB_EVENT_NAME="schedule")
        self.assertEqual(result.returncode, 2)
        self.assertEqual((output, calls), ("", []))


if __name__ == "__main__":
    unittest.main()
