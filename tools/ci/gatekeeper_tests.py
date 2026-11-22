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
        classifier = action.with_name("classify.sh")
        classifier.write_bytes(Path(runfiles.Create().Rlocation(
            "donner/.github/actions/pr-trust/classify.sh")).read_bytes())
        originals = (helper.read_bytes(), action.read_bytes())
        self.repo.rename(trusted / "pr-input")
        self.repo = trusted / "pr-input"
        payload = ("import os\nfrom pathlib import Path\nPath('INJECTED').touch()\n"
                   "target = Path(os.environ['GITHUB_WORKSPACE']) / "
                   "'.github/actions/pr-trust/action.yml'\n"
                   "target.write_text('forged trust action')\n")
        _commit(self.repo, ["tools/ci/gatekeeper.py"], payload)
        head = _commit(self.repo, [".github/actions/pr-trust/action.yml"], "forged action")
        self._test_merge(self.root, head)
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
        trust_body = 'bash "$GITHUB_ACTION_PATH/classify.sh"'
        self.assertIn("run: " + trust_body, action.read_text())
        env["GITHUB_ACTION_PATH"] = str(action.parent)
        self._run_fixture_shell(trust_body, env, trusted)
        self.assertIn("trusted_pr=false\n", output.read_text())
        env.update(TRUSTED_PR="false", SELFHOSTED_LINUX_RUNNER="true",
                   UNIFIED_RE_SELECTED="true")
        route = action.read_text().split("    - id: route\n", 1)[1]
        self._run_fixture_shell(textwrap.dedent(route.split("      run: |\n", 1)[1]), env, trusted)
        self.assertIn("use_self_hosted_linux=false\n", output.read_text())
        self.assertIn("unified_macos=false\n", output.read_text())

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

    def _test_merge(self, base_tip, *heads):
        """Checks out GitHub's test merge of `heads` into `base_tip`, as CI checks it out."""
        _git(self.repo, "checkout", "-q", "--detach", base_tip)
        _git(self.repo, "merge", "-q", "--no-ff", "-m", "test merge", *heads)
        return _git(self.repo, "rev-parse", "HEAD")

    def _docs_only(self, base, head, base_tip=None):
        """Runs docs-only on the test merge of `head` into `base_tip` (default: payload base)."""
        merge = self._test_merge(base_tip or base, head)
        result, output, _ = self._run("docs-only", **self._pull_request(base, head))
        self.assertEqual(result.returncode, 0, result.stderr)
        return result, output, merge

    def test_docs_only_pull_request(self):
        head = _commit(self.repo, ["docs/guide.md", "CHANGELOG.md", "NOTES.md", ".gitignore",
                                   ".editorconfig", ".gitattributes", "docs/img/a.svg"], "docs")
        result, output, merge = self._docs_only(self.root, head)
        self.assertEqual(output, "docs_only=true\n")
        self.assertIn(f"Comparing test merge {merge} with its first parent {self.root}.",
                      result.stdout)

    def test_any_build_input_clears_docs_only(self):
        for path in ("donner/BUILD.bazel", "nested/README.md", ".bazelignore", ".gitmodules",
                     "MODULE.bazel", "documentation.txt"):
            with self.subTest(path=path):
                _git(self.repo, "checkout", "-q", "--detach", self.root)
                head = _commit(self.repo, ["docs/guide.md", path], "change " + path)
                result, output, _ = self._docs_only(self.root, head)
                self.assertEqual(output, "docs_only=false\n")
                self.assertIn(f"Non-doc change: {path}", result.stdout)

    def test_cmake_generator_inputs_clear_docs_only(self):
        # A documentation-only pull request runs no CMake generator check (gen_cmakelists.py
        # --check runs only in CI's Linux jobs, which docs-only pull requests skip). That is safe
        # only while no path docs-only admits is a generator input: the Bazel build graph it
        # queries, the generator and its dependency map, CMake files and the module versions it
        # turns into FetchContent downloads.
        for path in ("tools/cmake/gen_cmakelists.py", "tools/cmake/external_deps.json",
                     "examples/cmake_consumer/CMakeLists.txt", "third_party/entt/CMakeLists.txt",
                     "tools/cmake/apply_woff2_patch.cmake", "CMakePresets.json",
                     "donner/base/BUILD.bazel", "donner/base/RcString.h", "build_defs/rules.bzl",
                     "MODULE.bazel", "third_party/bazel/non_bcr_deps.bzl"):
            with self.subTest(path=path):
                _git(self.repo, "checkout", "-q", "--detach", self.root)
                head = _commit(self.repo, ["docs/guide.md", path], "change " + path)
                _, output, _ = self._docs_only(self.root, head)
                self.assertEqual(output, "docs_only=false\n")

    def test_deleted_build_inputs_clear_docs_only(self):
        # A deleted build input can break the build (and the CMake generator check, which a
        # docs-only pull request skips) as surely as an edited one, so it is not a doc change.
        # A rename counts as deleting its old path, so moving a build input under docs/ is not
        # docs-only; deleting a document still is.
        for path, rename, expected in (("donner/old.cc", False, "false"),
                                       ("tools/cmake/external_deps.json", False, "false"),
                                       ("donner/base/BUILD.bazel", True, "false"),
                                       ("docs/old.md", False, "true")):
            with self.subTest(path=path, rename=rename):
                _git(self.repo, "checkout", "-q", "--detach", self.root)
                _commit(self.repo, [path], "add " + path)
                base = _git(self.repo, "rev-parse", "HEAD")
                if rename:
                    (Path(self.repo) / "docs").mkdir(exist_ok=True)
                    _git(self.repo, "mv", path, "docs/moved.txt")
                else:
                    _git(self.repo, "rm", "-q", path)
                head = _commit(self.repo, ["docs/guide.md"], "remove %s, edit docs" % path)
                _, output, _ = self._docs_only(base, head)
                self.assertEqual(output, "docs_only=%s\n" % expected)

    def test_base_drift_after_the_fork_is_not_attributed_to_the_pull_request(self):
        head = _commit(self.repo, ["docs/guide.md"], "docs")
        _git(self.repo, "checkout", "-q", "--detach", self.root)
        base_tip = _commit(self.repo, ["donner/code.cc"], "base drift")
        for payload_base in (self.root, base_tip):
            with self.subTest(payload_base=payload_base):
                _, output, _ = self._docs_only(payload_base, head, base_tip=base_tip)
                self.assertEqual(output, "docs_only=true\n")

    def test_a_payload_base_older_than_the_fork_keeps_a_docs_only_pull_request(self):
        # The payload's base commit lags the base branch: this head forked from a newer commit
        # with a code change. Only the test merge's first parent shows the pull request's own
        # change, which is documentation.
        _git(self.repo, "checkout", "-q", "--detach", self.root)
        fork = _commit(self.repo, ["donner/code.cc"], "base branch change")
        head = _commit(self.repo, ["docs/guide.md"], "docs")
        for payload_base in (self.root, fork):
            with self.subTest(payload_base=payload_base):
                _, output, _ = self._docs_only(payload_base, head, base_tip=fork)
                self.assertEqual(output, "docs_only=true\n")

    def test_a_docs_only_pull_request_that_merged_the_advanced_base_stays_docs_only(self):
        # The head forked before the base branch changed code, then merged that change; the base
        # branch moved on with another code change. The payload still records the fork point.
        head = _commit(self.repo, ["docs/guide.md"], "docs")
        _git(self.repo, "checkout", "-q", "--detach", self.root)
        advanced = _commit(self.repo, ["donner/code.cc"], "base branch change")
        _git(self.repo, "checkout", "-q", "--detach", head)
        _git(self.repo, "merge", "-q", "--no-ff", "-m", "merge the base branch", advanced)
        head = _commit(self.repo, ["docs/other.md"], "more docs")
        _git(self.repo, "checkout", "-q", "--detach", advanced)
        base_tip = _commit(self.repo, ["donner/later.cc"], "later base branch change")
        _, output, _ = self._docs_only(self.root, head, base_tip=base_tip)
        self.assertEqual(output, "docs_only=true\n")

    def test_restoring_a_path_to_an_older_base_commit_clears_docs_only(self):
        # The base branch changed donner/code.cc after the payload's base commit; the head, based
        # on that change, sets the file back to its content at the payload base and edits docs.
        # Against the payload base the file looks unchanged, but the tested tree changes it.
        before = _commit(self.repo, ["donner/code.cc"], "before")
        advanced = _commit(self.repo, ["donner/code.cc"], "after")
        (self.repo / "donner/code.cc").write_text("before\n", encoding="utf-8")
        _git(self.repo, "add", "--", "donner/code.cc")
        head = _commit(self.repo, ["docs/guide.md"], "restore donner/code.cc, edit docs")
        self.assertEqual(_git(self.repo, "rev-parse", f"{head}:donner/code.cc"),
                         _git(self.repo, "rev-parse", f"{before}:donner/code.cc"))
        result, output, _ = self._docs_only(before, head, base_tip=advanced)
        self.assertEqual(output, "docs_only=false\n")
        self.assertIn("Non-doc change: donner/code.cc", result.stdout)

    def test_an_empty_pull_request_with_an_older_payload_base_is_not_docs_only(self):
        # The head is a base-branch commit that adds documentation; the base branch has moved on.
        # The test merge changes nothing, though the older payload base sees the documentation.
        fork = _commit(self.repo, ["docs/guide.md"], "base branch docs")
        base_tip = _commit(self.repo, ["donner/code.cc"], "base branch code")
        # `git merge` would report the head as already merged; GitHub still builds a test merge.
        merge = _git(self.repo, "commit-tree", f"{base_tip}^{{tree}}", "-p", base_tip, "-p", fork,
                     "-m", "test merge")
        _git(self.repo, "checkout", "-q", "--detach", merge)
        result, output, _ = self._run("docs-only", **self._pull_request(self.root, fork))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, "docs_only=false\n")

    def test_empty_change_set_is_not_docs_only(self):
        _git(self.repo, "commit", "-q", "--allow-empty", "-m", "empty")
        head = _git(self.repo, "rev-parse", "HEAD")
        _, output, _ = self._docs_only(self.root, head)
        self.assertEqual(output, "docs_only=false\n")

    def test_a_checkout_that_is_not_a_test_merge_of_the_payload_head_is_not_docs_only(self):
        # Every head here changes only documentation, so only the binding keeps CI running.
        head = _commit(self.repo, ["docs/guide.md"], "docs")
        _git(self.repo, "checkout", "-q", "--detach", self.root)
        base_tip = _commit(self.repo, ["docs/base.md"], "base branch docs")
        _git(self.repo, "checkout", "-q", "--detach", self.root)
        other = _commit(self.repo, ["docs/other.md"], "another head")
        _git(self.repo, "checkout", "-q", "--detach", head)
        _git(self.repo, "merge", "-q", "--no-ff", "-m", "parents swapped", base_tip)
        swapped = _git(self.repo, "rev-parse", "HEAD")
        checkouts = {
            "the head itself": (head, head),
            "parents swapped": (swapped, head),
            "a merge of another head": (self._test_merge(base_tip, other), head),
            "three parents": (self._test_merge(base_tip, head, other), head),
            "an unknown payload head": (self._test_merge(base_tip, head), "0" * 40),
            "no payload head": (self._test_merge(base_tip, head), None),
        }
        for name, (checkout, payload_head) in checkouts.items():
            with self.subTest(checkout=name):
                _git(self.repo, "checkout", "-q", "--detach", checkout)
                context = self._pull_request(self.root, payload_head)
                if payload_head is None:
                    Path(context["GITHUB_EVENT_PATH"]).write_text(
                        json.dumps({"pull_request": {"base": {"sha": self.root}}}),
                        encoding="utf-8")
                result, output, _ = self._run("docs-only", **context)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(output, "docs_only=false\n")
                self.assertIn("Cannot bind the checkout to a test merge of the pull request head",
                              result.stdout)

    def test_a_checkout_without_the_merge_parents_is_not_docs_only(self):
        # actions/checkout's default depth does not fetch the test merge's parents.
        head = _commit(self.repo, ["docs/guide.md"], "docs")
        _git(self.repo, "branch", "test-merge", self._test_merge(self.root, head))
        for depth, expected in ((1, "false"), (2, "true")):
            with self.subTest(depth=depth):
                checkout = self.work / f"depth-{depth}"
                subprocess.run(["git", "clone", "-q", "--depth", str(depth), "--branch",
                                "test-merge", "file://" + str(self.repo), str(checkout)],
                               check=True, env=dict(os.environ, **_GIT_ENV),
                               timeout=test_timing.STALL_GUARD_SECONDS)
                result, output, _ = self._run("docs-only", cwd=checkout,
                                              **self._pull_request(self.root, head))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(output, f"docs_only={expected}\n")

    def test_a_git_failure_is_not_docs_only(self):
        head = _commit(self.repo, ["docs/guide.md"], "docs")
        merge = self._test_merge(self.root, head)
        not_a_repository = self.work / "not-a-repository"
        not_a_repository.mkdir()
        result, output, _ = self._run("docs-only", cwd=not_a_repository,
                                      **self._pull_request(self.root, head))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, "docs_only=false\n")
        # The first parent is listed but its tree is missing, so only the diff fails.
        tree = _git(self.repo, "rev-parse", f"{self.root}^{{tree}}")
        self.assertNotEqual(tree, _git(self.repo, "rev-parse", f"{merge}^{{tree}}"))
        (self.repo / ".git/objects" / tree[:2] / tree[2:]).unlink()
        result, output, _ = self._run("docs-only", **self._pull_request(self.root, head))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output, "docs_only=false\n")
        self.assertIn("git diff failed", result.stdout)

    def test_non_pull_request_events_are_never_docs_only(self):
        for event in ("push", "schedule", "workflow_dispatch"):
            with self.subTest(event=event):
                result, output, _ = self._run("docs-only", GITHUB_EVENT_NAME=event)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(output, "docs_only=false\n")

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
