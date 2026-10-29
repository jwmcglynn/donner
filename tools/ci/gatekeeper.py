#!/usr/bin/env python3
"""Run-admission decisions shared by the workflows' gatekeeper jobs.

Each subcommand reads the GitHub Actions run context from the environment the runner provides
(`GITHUB_EVENT_NAME`, `GITHUB_REF`, `GITHUB_EVENT_PATH`, ...) and appends one `name=value` line
to `$GITHUB_OUTPUT`, so a workflow step is a single invocation and the same rendered step runs
for every event. Who may run what (trust and secrets) stays in the workflow itself.

  duplicate-push  `is_duplicate_push=true` for a push to a non-main branch that an open pull
                  request already covers. That push and the pull request's synchronize event
                  would otherwise run CI twice at the same commit; the push run then skips
                  cleanly instead of being cancelled. Needs `GH_TOKEN` for `gh pr list`.
  docs-only       `docs_only=true` for a pull request whose changes, since its merge base, are
                  all documentation. Anything that is not a pull request is never docs-only.
  main-advanced --workflow FILE
                  `should_run=true` for a manual dispatch, or when origin/main has moved since
                  FILE's last successful run (or FILE has none). A scheduled nightly then skips
                  a commit it has already passed. Needs `GH_TOKEN` for `gh run list`.

The `gh` calls share the repository's hourly API budget with every other workflow. A call the
rate limit refuses is repeated after the window resets, within github_rate_limit's cap; past
the cap the step fails rather than guess an answer.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from collections.abc import Callable, Iterable, Mapping
from pathlib import Path

# The shared rate-limit helper sits beside this script; the admission checkout includes it. The
# directory is added explicitly because an interpreter in safe-path mode does not add it.
sys.path.insert(0, str(Path(__file__).absolute().parent))
import github_rate_limit  # noqa: E402

MAIN_REF = "refs/heads/main"

# Top-level files besides `*.md` that never affect a build. Build-impacting dotfiles such as
# .bazelignore (Bazel package discovery) and .gitmodules (vendored dependencies) are deliberately
# absent, so a change to one of them runs the full CI.
_DOCS_ONLY_FILES = (".editorconfig", ".gitattributes", ".gitignore")


def is_docs_only_path(path: str) -> bool:
    """True for a changed path that cannot affect any build or test."""
    if path.startswith("docs/") or path.startswith("CHANGELOG") or path in _DOCS_ONLY_FILES:
        return True
    return "/" not in path and path.endswith(".md")


def _write_output(environ: Mapping[str, str], name: str, value: bool) -> None:
    with open(environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
        output.write(f"{name}={'true' if value else 'false'}\n")


def _run(command: list[str]) -> str:
    """Runs `command` and returns its stdout; a failure ends the step with that command's status.

    A `gh` command is repeated after a rate-limit refusal (github_rate_limit.run_gh).
    """
    if command[0] == "gh":
        return github_rate_limit.run_gh(command, stdout=subprocess.PIPE, text=True,
                                        label="gatekeeper").stdout
    return subprocess.run(command, stdout=subprocess.PIPE, text=True, check=True).stdout


def duplicate_push(environ: Mapping[str, str], run: Callable[[list[str]], str] = _run) -> bool:
    if environ["GITHUB_EVENT_NAME"] != "push" or environ["GITHUB_REF"] == MAIN_REF:
        return False
    branch = environ["GITHUB_REF_NAME"]
    count = run(["gh", "pr", "list", "--repo", environ["GITHUB_REPOSITORY"],
                 "--state", "open", "--head", branch, "--json", "number", "-q", "length"])
    try:
        open_prs = int(count.strip())
    except ValueError:
        # Anything but a count leaves the push run in place: CI runs rather than skips.
        return False
    if open_prs > 0:
        print(f"Skipping push CI: open PR already covers branch '{branch}'")
        return True
    return False


def _diff_base(base_sha: str, head_sha: str) -> str:
    # base.sha is the base branch's current tip, so a two-dot diff from it would attribute
    # post-fork drift on the base to the pull request. Diff from the merge base instead. If it
    # cannot be computed, fall back to the base tip: drift can then only clear docs_only, which
    # runs more CI, never less.
    merge_base = subprocess.run(["git", "merge-base", base_sha, head_sha],
                                stdout=subprocess.PIPE, text=True, check=False)
    if merge_base.returncode == 0:
        diff_base = merge_base.stdout.strip()
        print(f"Using merge-base {diff_base} as diff basis.")
        return diff_base
    print(f"git merge-base failed; using base tip {base_sha} as diff basis.")
    return base_sha


def docs_only(environ: Mapping[str, str]) -> bool:
    if environ["GITHUB_EVENT_NAME"] != "pull_request":
        return False
    event = json.loads(Path(environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
    base_sha = event["pull_request"]["base"]["sha"]
    head_sha = event["pull_request"]["head"]["sha"]
    changed = _run(["git", "diff", "--name-only", "--diff-filter=ACMR",
                    _diff_base(base_sha, head_sha), head_sha]).splitlines()
    return _all_docs(path for path in changed if path)


def _all_docs(paths: Iterable[str]) -> bool:
    any_path = False
    for path in paths:
        any_path = True
        if not is_docs_only_path(path):
            print(f"Non-doc change: {path}")
            return False
    # An empty change set is not evidence of a docs-only change.
    return any_path


def main_advanced(environ: Mapping[str, str], workflow: str) -> bool:
    if environ["GITHUB_EVENT_NAME"] == "workflow_dispatch":
        print(f"Manual dispatch requested; running {workflow}.")
        return True
    subprocess.run(["git", "fetch", "--no-tags", "--depth=1", "origin", "main"], check=True)
    main_sha = _run(["git", "rev-parse", "origin/main"]).strip()
    last_success = _run(["gh", "run", "list", f"--workflow={workflow}", "--status=success",
                         "--limit=1", "--json", "headSha", "--jq", ".[0].headSha"]).strip()
    if last_success in ("", "null"):
        print(f"No previous successful {workflow} run found; running.")
        return True
    if last_success == main_sha:
        print(f"origin/main has not advanced since the last successful {workflow} run; skipping.")
        return False
    print(f"origin/main advanced from {last_success} to {main_sha}; running {workflow}.")
    return True


def main(argv: list[str] | None = None, environ: Mapping[str, str] = os.environ) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    decisions = parser.add_subparsers(dest="decision", required=True)
    decisions.add_parser("duplicate-push")
    decisions.add_parser("docs-only")
    advanced = decisions.add_parser("main-advanced")
    advanced.add_argument("--workflow", required=True,
                          help="workflow file whose last successful run is compared, e.g. perf.yml")
    args = parser.parse_args(argv)
    if args.decision == "duplicate-push":
        _write_output(environ, "is_duplicate_push", duplicate_push(environ))
    elif args.decision == "docs-only":
        _write_output(environ, "docs_only", docs_only(environ))
    else:
        _write_output(environ, "should_run", main_advanced(environ, args.workflow))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
    except github_rate_limit.RateLimitExhausted:
        sys.exit(1)
