#!/usr/bin/env python3
"""Determine CI Bazel targets and emit GitHub outputs plus diagnostics."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path


INFRA_PATTERNS = (
    "MODULE.bazel",
    "WORKSPACE",
    "WORKSPACE.",
    ".bazelversion",
    ".bazelrc",
    "build_defs/",
    "tools/ci_determine_targets.py",
    "tools/ci_diagnostics.sh",
    "tools/ci_diagnostics_report.py",
    "tools/coverage.sh",
    ".github/workflows/",
    ".github/actions/",
)


def write_github_output(path: Path | None, values: dict[str, str]) -> None:
    if path is None:
        return

    with path.open("a", encoding="utf-8") as output:
        for key, value in values.items():
            if "\n" in value:
                output.write(f"{key}<<__DONNER_CI_OUTPUT__\n{value}\n__DONNER_CI_OUTPUT__\n")
            else:
                output.write(f"{key}={value}\n")


def run_command(
    args: list[str],
    cwd: Path,
    stdout_path: Path,
    stderr_path: Path,
) -> None:
    with stdout_path.open("w", encoding="utf-8") as stdout, stderr_path.open(
        "w", encoding="utf-8"
    ) as stderr:
        subprocess.run(args, cwd=cwd, check=True, stdout=stdout, stderr=stderr)


def git_lines(args: list[str], cwd: Path) -> list[str]:
    result = subprocess.run(args, cwd=cwd, check=True, capture_output=True, text=True)
    return [line for line in result.stdout.splitlines() if line]


def fallback(reason: str, diagnostics_dir: Path, github_output: Path | None) -> int:
    diagnostics_dir.mkdir(parents=True, exist_ok=True)
    (diagnostics_dir / "affected-targets.txt").write_text("", encoding="utf-8")
    (diagnostics_dir / "target-selection.env").write_text(
        f"mode=full\nfallback=true\nreason={reason}\ntarget_count=1\n",
        encoding="utf-8",
    )
    write_github_output(
        github_output,
        {
            "fallback": "true",
            "affected": "",
            "mode": "full",
            "reason": reason,
        },
    )
    print(f"Target selection: full //... ({reason})")
    return 0


def is_infra_path(path: str) -> bool:
    for pattern in INFRA_PATTERNS:
        if pattern.endswith("/"):
            if path.startswith(pattern):
                return True
            continue
        if pattern.endswith("."):
            if path.startswith(pattern):
                return True
            continue
        if path == pattern:
            return True
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bazel-diff-jar", required=True, type=Path)
    parser.add_argument("--diagnostics-dir", required=True, type=Path)
    parser.add_argument("--github-output", type=Path, default=None)
    args = parser.parse_args()

    repo = Path.cwd()
    diagnostics_dir = args.diagnostics_dir
    diagnostics_dir.mkdir(parents=True, exist_ok=True)

    event_name = os.environ.get("GITHUB_EVENT_NAME", "")
    event_path = os.environ.get("GITHUB_EVENT_PATH", "")
    if event_name != "pull_request":
        return fallback("non_pull_request_event", diagnostics_dir, args.github_output)
    if not event_path:
        return fallback("missing_event_payload", diagnostics_dir, args.github_output)

    event = json.loads(Path(event_path).read_text(encoding="utf-8"))
    pr = event.get("pull_request") or {}
    base_sha = ((pr.get("base") or {}).get("sha")) or ""
    head_sha = ((pr.get("head") or {}).get("sha")) or ""
    labels = {label.get("name", "") for label in pr.get("labels", [])}

    if not base_sha or not head_sha:
        return fallback("missing_base_or_head_sha", diagnostics_dir, args.github_output)
    if "ci:full-test" in labels:
        return fallback("full_test_label", diagnostics_dir, args.github_output)

    changed_files = git_lines(["git", "diff", "--name-only", base_sha, head_sha], repo)
    (diagnostics_dir / "changed-files.txt").write_text(
        "\n".join(changed_files) + ("\n" if changed_files else ""),
        encoding="utf-8",
    )

    if not changed_files:
        return fallback("no_changed_files", diagnostics_dir, args.github_output)

    for path in changed_files:
        if is_infra_path(path):
            (diagnostics_dir / "affected-targets.txt").write_text("", encoding="utf-8")
            (diagnostics_dir / "target-selection.env").write_text(
                f"mode=full\nfallback=true\nreason=infra_change\ninfra_path={path}\ntarget_count=1\n",
                encoding="utf-8",
            )
            write_github_output(
                args.github_output,
                {
                    "fallback": "true",
                    "affected": "",
                    "mode": "full",
                    "reason": "infra_change",
                },
            )
            print(f"Target selection: full //... (infra change: {path})")
            return 0

    work_dir = Path(tempfile.mkdtemp(prefix="donner-ci-targets-", dir=os.environ.get("RUNNER_TEMP")))
    timings: list[tuple[str, float]] = []

    def timed(name: str, fn) -> None:
        start = time.monotonic()
        fn()
        timings.append((name, time.monotonic() - start))

    try:
        base_dir = work_dir / "base"
        head_dir = work_dir / "head"
        timed(
            "worktree_base",
            lambda: subprocess.run(
                ["git", "worktree", "add", "--detach", str(base_dir), base_sha],
                cwd=repo,
                check=True,
            ),
        )
        timed(
            "worktree_head",
            lambda: subprocess.run(
                ["git", "worktree", "add", "--detach", str(head_dir), head_sha],
                cwd=repo,
                check=True,
            ),
        )

        timed(
            "bazel_diff_base_hashes",
            lambda: run_command(
                [
                    "java",
                    "-jar",
                    str(args.bazel_diff_jar),
                    "generate-hashes",
                    "-w",
                    str(base_dir),
                    "-b",
                    "bazelisk",
                    str(work_dir / "base-hashes.json"),
                ],
                repo,
                diagnostics_dir / "bazel-diff-base.stdout.log",
                diagnostics_dir / "bazel-diff-base.stderr.log",
            ),
        )
        timed(
            "bazel_diff_head_hashes",
            lambda: run_command(
                [
                    "java",
                    "-jar",
                    str(args.bazel_diff_jar),
                    "generate-hashes",
                    "-w",
                    str(head_dir),
                    "-b",
                    "bazelisk",
                    str(work_dir / "head-hashes.json"),
                ],
                repo,
                diagnostics_dir / "bazel-diff-head.stdout.log",
                diagnostics_dir / "bazel-diff-head.stderr.log",
            ),
        )
        affected_file = diagnostics_dir / "affected-targets.txt"
        timed(
            "bazel_diff_impacted_targets",
            lambda: run_command(
                [
                    "java",
                    "-jar",
                    str(args.bazel_diff_jar),
                    "get-impacted-targets",
                    "-w",
                    str(head_dir),
                    "-b",
                    "bazelisk",
                    "-sh",
                    str(work_dir / "base-hashes.json"),
                    "-fh",
                    str(work_dir / "head-hashes.json"),
                    "-o",
                    str(affected_file),
                ],
                repo,
                diagnostics_dir / "bazel-diff-impacted.stdout.log",
                diagnostics_dir / "bazel-diff-impacted.stderr.log",
            ),
        )
    except subprocess.CalledProcessError as exc:
        print(f"bazel-diff failed; falling back to //...: {exc}", file=sys.stderr)
        return fallback("bazel_diff_failed", diagnostics_dir, args.github_output)
    finally:
        for child in (work_dir / "base", work_dir / "head"):
            subprocess.run(
                ["git", "worktree", "remove", "--force", str(child)],
                cwd=repo,
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        shutil.rmtree(work_dir, ignore_errors=True)

    affected_file = diagnostics_dir / "affected-targets.txt"
    affected = [line.strip() for line in affected_file.read_text(encoding="utf-8").splitlines() if line.strip()]
    if not affected:
        return fallback("no_impacted_targets", diagnostics_dir, args.github_output)

    affected_one_line = " ".join(affected)
    (diagnostics_dir / "timings.tsv").write_text(
        "".join(f"{name}\t{seconds:.3f}\n" for name, seconds in timings),
        encoding="utf-8",
    )
    (diagnostics_dir / "target-selection.env").write_text(
        f"mode=affected\nfallback=false\nreason=bazel_diff\ntarget_count={len(affected)}\n",
        encoding="utf-8",
    )
    write_github_output(
        args.github_output,
        {
            "fallback": "false",
            "affected": affected_one_line,
            "mode": "affected",
            "reason": "bazel_diff",
        },
    )

    print(f"Target selection: affected targets ({len(affected)})")
    for label in affected[:40]:
        print(label)
    if len(affected) > 40:
        print(f"... {len(affected) - 40} more labels written to {affected_file}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
