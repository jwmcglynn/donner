#!/usr/bin/env python3
"""Build a rename map from the old resvg-test-suite pin to the current HEAD.

For each .svg that existed in the old pinned commit, chain through git's rename
events across the intervening commits to determine its current path (or mark it
as deleted).

Usage:
    build_rename_map.py --repo /path/to/upstream/resvg-test-suite \
        --old-commit 682a9c8da8c580ad59cba0ef8cb8a8fd5534022f \
        --new-commit HEAD \
        --out rename_map.json

Strategy:
    1. List every svg/*.svg file in the old commit.
    2. Collect all rename events (R<similarity> src dst) between the old and
       new commit from `git log --find-renames=5% --diff-filter=R`.
    3. For each old path, walk the rename chain forward until it stabilizes.
    4. Verify the final path exists in the new commit tree. If it doesn't,
       the file was deleted; mark as null.
    5. Report renames with similarity < 80% as "low_confidence" for manual
       review (these are chained but may be wrong).
"""

import argparse
import json
import os
import re
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class RenameEvent:
    commit: str
    src: str
    dst: str
    similarity: int  # 0-100


def run_git(repo: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout


def list_old_svgs(repo: Path, commit: str) -> list[str]:
    """List every .svg under svg/ in the old commit."""
    out = run_git(repo, "ls-tree", "-r", "--name-only", commit, "svg/")
    return sorted(p for p in out.splitlines() if p.endswith(".svg"))


def list_new_tree(repo: Path, commit: str) -> set[str]:
    """Return the set of every path in the new commit (for existence checks)."""
    out = run_git(repo, "ls-tree", "-r", "--name-only", commit)
    return set(out.splitlines())


def collect_rename_events(repo: Path, old: str, new: str) -> list[RenameEvent]:
    """Collect every rename event in [old..new] in chronological order (old→new).

    Uses --find-renames=5% to catch aggressive moves (e.g. flat→hierarchical).
    Returns events in chronological order (oldest first).
    """
    out = run_git(
        repo,
        "log",
        "--find-renames=5%",
        "--diff-filter=R",
        "--name-status",
        "--format=COMMIT:%H",
        "--reverse",  # chronological order
        f"{old}..{new}",
    )

    events: list[RenameEvent] = []
    current_commit = ""
    rename_re = re.compile(r"^R(\d+)\t([^\t]+)\t([^\t]+)$")

    for line in out.splitlines():
        line = line.rstrip()
        if line.startswith("COMMIT:"):
            current_commit = line[len("COMMIT:"):]
            continue
        if not line or line.startswith(" "):
            continue
        m = rename_re.match(line)
        if m:
            similarity = int(m.group(1))
            src = m.group(2)
            dst = m.group(3)
            events.append(RenameEvent(current_commit, src, dst, similarity))

    return events


def build_rename_chains(
    old_paths: list[str],
    events: list[RenameEvent],
    new_tree: set[str],
) -> tuple[dict[str, str | None], list[dict]]:
    """For each old path, walk forward through rename events.

    Returns:
      - map: old_path → new_path (or None if deleted)
      - low_confidence: list of {old, new, min_similarity, hops} for review
    """
    # Group events by source path → list of (dst, similarity, commit)
    # Ordered chronologically, so first match is the earliest rename.
    by_src: dict[str, list[RenameEvent]] = defaultdict(list)
    for ev in events:
        by_src[ev.src].append(ev)

    result: dict[str, str | None] = {}
    low_confidence: list[dict] = []

    for old_path in old_paths:
        current = old_path
        min_similarity = 100
        hops = 0
        visited = {current}

        while True:
            if current not in by_src:
                break
            # Take the first (earliest) rename event for this path.
            # A path can only be renamed once (it stops existing after).
            ev = by_src[current][0]
            min_similarity = min(min_similarity, ev.similarity)
            current = ev.dst
            hops += 1
            if current in visited:
                # Shouldn't happen — git history is a DAG — but guard anyway.
                break
            visited.add(current)

        if current in new_tree:
            result[old_path] = current
        else:
            # Final destination doesn't exist in new tree → deleted somewhere.
            result[old_path] = None

        if hops > 0 and min_similarity < 80 and result[old_path] is not None:
            low_confidence.append({
                "old": old_path,
                "new": current,
                "min_similarity": min_similarity,
                "hops": hops,
            })

    return result, low_confidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True,
                        help="Path to upstream resvg-test-suite clone")
    parser.add_argument("--old-commit", required=True,
                        help="Pinned commit we're migrating from")
    parser.add_argument("--new-commit", default="HEAD",
                        help="Commit we're migrating to (default: HEAD)")
    parser.add_argument("--out", type=Path, required=True,
                        help="Path to write rename_map.json")
    args = parser.parse_args()

    if not args.repo.exists():
        print(f"error: repo not found: {args.repo}", file=sys.stderr)
        return 1

    print(f"Listing old svgs at {args.old_commit[:12]}...", file=sys.stderr)
    old_svgs = list_old_svgs(args.repo, args.old_commit)
    print(f"  {len(old_svgs)} svgs in old tree", file=sys.stderr)

    print(f"Listing new tree at {args.new_commit}...", file=sys.stderr)
    new_tree = list_new_tree(args.repo, args.new_commit)
    print(f"  {len(new_tree)} paths in new tree", file=sys.stderr)

    print("Collecting rename events...", file=sys.stderr)
    events = collect_rename_events(args.repo, args.old_commit, args.new_commit)
    print(f"  {len(events)} rename events across range", file=sys.stderr)

    print("Building rename chains...", file=sys.stderr)
    mapping, low_confidence = build_rename_chains(old_svgs, events, new_tree)

    migrated = sum(1 for v in mapping.values() if v is not None)
    orphaned = sum(1 for v in mapping.values() if v is None)

    print(f"\nResults:", file=sys.stderr)
    print(f"  migrated:       {migrated}", file=sys.stderr)
    print(f"  orphaned:       {orphaned}", file=sys.stderr)
    print(f"  low confidence: {len(low_confidence)} (similarity < 80%)",
          file=sys.stderr)

    # Sort for deterministic output.
    output = {
        "old_commit": args.old_commit,
        "new_commit": run_git(args.repo, "rev-parse", args.new_commit).strip(),
        "total_old_svgs": len(old_svgs),
        "migrated": migrated,
        "orphaned": orphaned,
        "mapping": dict(sorted(mapping.items())),
        "low_confidence": sorted(
            low_confidence, key=lambda e: e["min_similarity"]),
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(output, indent=2) + "\n")
    print(f"\nWrote {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
