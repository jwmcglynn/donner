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
  docs-only       `docs_only=true` for a pull request whose test merge, the checkout in the
                  working directory, changes only documentation relative to its first parent.
                  Any other event, a checkout that is not that merge and a git failure are
                  never docs-only.
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


def _git(*args: str) -> str | None:
    """Runs git and returns its stdout, or None when git fails."""
    result = subprocess.run(["git", *args], stdout=subprocess.PIPE, text=True, check=False)
    return result.stdout if result.returncode == 0 else None


def _tested_merge(head_sha: str) -> tuple[str, str] | None:
    """Returns the checked-out test merge and its first parent, or None if it cannot be bound.

    CI builds GitHub's merge of the pull request head into the base branch tip, so the merge's
    first parent is the tested base; the event payload's base commit can lag it. Only a merge
    with exactly two parents whose second is the payload's head is bound. A shallow checkout
    lists no parents, so it is not.
    """
    ancestry = _git("rev-list", "--parents", "-n", "1", "HEAD")
    commits = ancestry.split() if ancestry is not None else []
    if len(commits) != 3 or commits[2] != head_sha:
        print(f"Cannot bind the checkout to a test merge of the pull request head {head_sha}: "
              f"HEAD and its parents are {commits or 'unavailable'}.")
        return None
    return commits[0], commits[1]


def docs_only(environ: Mapping[str, str]) -> bool:
    if environ["GITHUB_EVENT_NAME"] != "pull_request":
        return False
    event = json.loads(Path(environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
    head_sha = event.get("pull_request", {}).get("head", {}).get("sha") or ""
    # The checkout is PR-controlled data: it is read through git only, never executed. Anything
    # that does not bind to the tested merge, and any git failure, runs CI.
    tested = _tested_merge(head_sha)
    if tested is None:
        return False
    merge, first_parent = tested
    print(f"Comparing test merge {merge} with its first parent {first_parent}.")
    # Every changed path counts, whatever its status: deleting a build input (a source, a BUILD
    # file, a CMake file) can break the build as surely as editing it. --no-renames reports a
    # rename as its deleted old path and its added new path, so moving a build input under docs/
    # is not docs-only either.
    changed = _git("diff", "--raw", "--no-renames", first_parent, merge)
    if changed is None:
        print("git diff failed; not docs-only.")
        return False
    return _all_docs_entries(changed)


# Without rename, copy or break detection, a diff between two commits reports only these.
_TREE_DIFF_STATUSES = ("A", "D", "M", "T")
# Absent, and regular files. A symlink or gitlink is never documentation, even under docs/: the
# directory is not in .bazelignore, so a symlink there can add packages to Bazel's `//...`.
_DOCUMENT_MODES = ("000000", "100644", "100755")


def _all_docs_entries(raw_diff: str) -> bool:
    """True when every `git diff --raw` entry is a documentation file; unknown entries are not."""
    paths = []
    for line in raw_diff.splitlines():
        metadata, tab, path = line.partition("\t")
        fields = metadata.split()
        if not tab or len(fields) != 5 or fields[4] not in _TREE_DIFF_STATUSES:
            print(f"Unexpected diff entry: {line}")
            return False
        if fields[0].removeprefix(":") not in _DOCUMENT_MODES or fields[1] not in _DOCUMENT_MODES:
            print(f"Non-file change: {path}")
            return False
        paths.append(path)
    return _all_docs(paths)


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
