#!/usr/bin/env python3
"""Bound the hosted CI Bazel disk-cache snapshot and probe runner resources.

The hosted CI lanes restore a Bazel `--disk_cache` directory from a GitHub Actions cache and
main pushes save it again under the new revision. Without eviction the snapshot only grows,
and a hosted runner has a small disk, so every restore and save moves more bytes through it.

`bound` evicts least recently used entries until the saved archive fits a fixed budget, using
the policy of Bazel's own disk-cache garbage collector (`DiskCacheGarbageCollector`): sort
entries by mtime, then path, and delete from the oldest. Bazel sets an entry's mtime whenever
it stores or retrieves it, and an action-cache hit refreshes the action result before every
blob it references, so evicting in this order never leaves an action result whose blobs are
gone. Bazel's own collector runs only as an idle task that the next command interrupts, so it
cannot be relied on to finish before the save step; this script runs the same policy
synchronously. The budget is measured as the uncompressed tar stream (512-byte header plus
512-byte-padded data per file), which bounds the compressed archive the save uploads. Any
error, or a cache still over budget after eviction, reports `save=false`: an unsaved snapshot
costs one cold restore, a huge one costs the runner.

`probe` prints disk, cache-directory and memory usage. It is diagnostic only: it walks each
directory against a deadline, bounds every command it runs, and always exits 0.

Usage:
  ci_disk_cache.py bound --cache-dir DIR --max-mib N [--marker FILE] [--dry-run]
                         [--github-output FILE]
  ci_disk_cache.py probe --stage NAME [--cache-dir DIR] [--marker FILE]

Stdlib only.
"""

import argparse
import os
import platform
import subprocess
import sys
import time

TAR_BLOCK = 512
# The end-of-archive marker is two zero blocks.
TAR_TRAILER = 2 * TAR_BLOCK
# Bazel's in-flight writes. An idle server leaves only orphans of interrupted writes there.
TMP_DIR = "tmp"
PROBE_WALK_SECONDS = 20
PROBE_COMMAND_SECONDS = 15
MIB = 1024 * 1024


def tar_bytes(size):
    """Bytes one regular file adds to an uncompressed tar stream."""
    return TAR_BLOCK + -(-size // TAR_BLOCK) * TAR_BLOCK


class Entry:
    __slots__ = ("path", "size", "mtime_ns", "temporary")

    def __init__(self, path, size, mtime_ns, temporary):
        self.path = path
        self.size = size
        self.mtime_ns = mtime_ns
        self.temporary = temporary


def scan(root):
    """Every regular file and directory under root, without following symlinks.

    Returns (entries, directory_count, other_count). Symlinks and special files are counted but
    never selected for deletion; a well-formed disk cache holds none.
    """
    entries = []
    directories = 0
    others = 0
    tmp_root = os.path.join(root, TMP_DIR)
    stack = [root]
    while stack:
        current = stack.pop()
        directories += 1
        with os.scandir(current) as iterator:
            for dirent in iterator:
                if dirent.is_dir(follow_symlinks=False):
                    stack.append(dirent.path)
                elif dirent.is_file(follow_symlinks=False):
                    try:
                        status = dirent.stat(follow_symlinks=False)
                    except FileNotFoundError:
                        continue
                    temporary = os.path.commonpath([tmp_root, dirent.path]) == tmp_root
                    entries.append(Entry(dirent.path, status.st_size, status.st_mtime_ns,
                                         temporary))
                else:
                    others += 1
    return entries, directories, others


def archive_estimate(entries, directories, others):
    """Uncompressed tar stream size for these entries; directories and links take one header."""
    return (sum(tar_bytes(entry.size) for entry in entries)
            + (directories + others) * TAR_BLOCK + TAR_TRAILER)


def eviction_order(entries):
    """Entries in deletion order: orphaned temporaries first, then oldest mtime, then path.

    The path tie break is Bazel's: `ac/` sorts before `cas/`, so an action result goes before
    the blobs it references when their mtimes are equal.
    """
    return sorted(entries, key=lambda entry: (not entry.temporary, entry.mtime_ns, entry.path))


def select_evictions(entries, directories, others, budget):
    """The shortest oldest-first prefix whose removal brings the archive within budget."""
    excess = archive_estimate(entries, directories, others) - budget
    evict = []
    for entry in eviction_order(entries):
        if excess <= 0 and not entry.temporary:
            break
        evict.append(entry)
        excess -= tar_bytes(entry.size)
    return evict


def marker_ns(marker):
    if not marker:
        return None
    try:
        return os.stat(marker).st_mtime_ns
    except OSError:
        return None


def mib(value):
    return "%.1f MiB" % (value / MIB)


def describe(label, entries, directories, others, since_ns):
    total = sum(entry.size for entry in entries)
    line = "%s: %d files, %s logical, %s as tar" % (
        label, len(entries), mib(total), mib(archive_estimate(entries, directories, others)))
    if since_ns is not None:
        recent = [entry for entry in entries if entry.mtime_ns > since_ns]
        line += "; used or written since restore: %d files, %s" % (
            len(recent), mib(sum(entry.size for entry in recent)))
    return line


def write_output(path, name, value):
    if path:
        with open(path, "a", encoding="utf-8") as stream:
            stream.write("%s=%s\n" % (name, value))


def bound(args):
    """Evict to the budget. Returns the value of the `save` output."""
    budget = args.max_mib * MIB
    root = os.path.abspath(os.path.expanduser(args.cache_dir))
    if not os.path.isdir(root):
        print("Disk cache %s does not exist; nothing to save." % root)
        return False
    since_ns = marker_ns(args.marker)
    entries, directories, others = scan(root)
    print(describe("Disk cache before bounding", entries, directories, others, since_ns))
    evict = select_evictions(entries, directories, others, budget)
    kept_entries = len(entries) - len(evict)
    print("Budget %s as tar: %s %d files (%s), keeping %d." % (
        mib(budget), "would evict" if args.dry_run else "evicting", len(evict),
        mib(sum(entry.size for entry in evict)), kept_entries))
    if args.dry_run:
        print("Dry run: not a main push, so nothing is deleted or saved.")
        return False
    for entry in evict:
        try:
            os.unlink(entry.path)
        except FileNotFoundError:
            pass
    entries, directories, others = scan(root)
    estimate = archive_estimate(entries, directories, others)
    print(describe("Disk cache after bounding", entries, directories, others, since_ns))
    if estimate > budget:
        print("::warning title=Disk cache over budget::The Bazel disk cache is %s as tar after "
              "eviction, over its %s budget; this run does not save it." % (
                  mib(estimate), mib(budget)))
        return False
    return True


def run_bound(args):
    try:
        save = bound(args)
    except Exception as error:  # Fail safe: any error means the snapshot is not saved.
        print("::warning title=Disk cache not bounded::Bounding the Bazel disk cache failed "
              "(%s: %s); this run does not save it." % (type(error).__name__, error))
        save = False
    write_output(args.github_output, "save", "true" if save else "false")
    return 0


def walk_usage(root, deadline):
    """(files, logical bytes, allocated bytes, oldest mtime, newest mtime, complete)."""
    files = logical = allocated = 0
    oldest = newest = None
    stack = [root]
    while stack:
        if time.monotonic() > deadline:
            return files, logical, allocated, oldest, newest, False
        current = stack.pop()
        try:
            iterator = os.scandir(current)
        except OSError:
            continue
        with iterator:
            for dirent in iterator:
                try:
                    status = dirent.stat(follow_symlinks=False)
                except OSError:
                    continue
                allocated += getattr(status, "st_blocks", 0) * 512
                if dirent.is_dir(follow_symlinks=False):
                    stack.append(dirent.path)
                elif dirent.is_file(follow_symlinks=False):
                    files += 1
                    logical += status.st_size
                    mtime = status.st_mtime
                    oldest = mtime if oldest is None else min(oldest, mtime)
                    newest = mtime if newest is None else max(newest, mtime)
    return files, logical, allocated, oldest, newest, True


def probe_directory(label, path, marker):
    path = os.path.expanduser(path)
    if not os.path.isdir(path):
        print("%s (%s): absent" % (label, path))
        return
    files, logical, allocated, oldest, newest, complete = walk_usage(
        path, time.monotonic() + PROBE_WALK_SECONDS)
    line = "%s (%s): %d files, %s logical, %s allocated" % (
        label, path, files, mib(logical), mib(allocated))
    if oldest is not None:
        now = time.time()
        line += ", newest file %.0f s old, oldest %.0f s old" % (now - newest, now - oldest)
    since_ns = marker_ns(marker)
    if since_ns is not None:
        line += ", restore marker %.0f s old" % (time.time() - since_ns / 1e9)
    if not complete:
        line += " (partial: stopped after %d s)" % PROBE_WALK_SECONDS
    print(line)


def probe_command(argv):
    try:
        result = subprocess.run(argv, capture_output=True, text=True,
                                timeout=PROBE_COMMAND_SECONDS)
        output = (result.stdout + result.stderr).rstrip()
    except (OSError, subprocess.SubprocessError) as error:
        output = "unavailable (%s)" % type(error).__name__
    print("$ %s\n%s" % (" ".join(argv), output))


def output_root():
    user = os.environ.get("USER") or os.environ.get("LOGNAME") or ""
    if platform.system() == "Darwin":
        return "/private/var/tmp/_bazel_%s" % user
    return "~/.cache/bazel"


def probe(args):
    print("::group::Runner resources: %s" % args.stage)
    probe_command(["df", "-h"])
    if platform.system() == "Linux":
        probe_command(["df", "-ih", "/"])
    probe_directory("Bazel disk cache", args.cache_dir, args.marker)
    probe_directory("Bazel repository cache", "~/.cache/bazel-repo", None)
    probe_directory("Bazel output root", output_root(), None)
    if platform.system() == "Darwin":
        probe_command(["sysctl", "hw.memsize", "vm.swapusage"])
        probe_command(["vm_stat"])
    else:
        probe_command(["free", "-m"])
    print("::endgroup::")


def run_probe(args):
    try:
        probe(args)
    except Exception as error:  # Diagnostic only; never fail the job.
        print("Runner resource probe failed: %s: %s" % (type(error).__name__, error))
    return 0


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    commands = parser.add_subparsers(dest="command", required=True)
    bound_parser = commands.add_parser("bound", help="evict to the snapshot budget")
    bound_parser.add_argument("--cache-dir", required=True)
    bound_parser.add_argument("--max-mib", type=int, required=True)
    bound_parser.add_argument("--marker")
    bound_parser.add_argument("--dry-run", action="store_true")
    bound_parser.add_argument("--github-output")
    probe_parser = commands.add_parser("probe", help="print disk and memory usage")
    probe_parser.add_argument("--stage", required=True)
    probe_parser.add_argument("--cache-dir", default="~/.cache/bazel-disk")
    probe_parser.add_argument("--marker")
    args = parser.parse_args(argv)
    if args.command == "bound" and args.max_mib <= 0:
        parser.error("--max-mib must be positive")
    return args


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.command == "bound":
        return run_bound(args)
    return run_probe(args)


if __name__ == "__main__":
    sys.exit(main())
