#!/usr/bin/env python3
"""Stages the Editor WASM workflow's package handoff and immutable deployment candidate.

Each subcommand is one step of the workflow's build job. It runs from the repository root, reads
the run context from the variables the runner provides and writes step outputs to $GITHUB_OUTPUT,
so the workflow step is a single invocation. Uploading what it stages stays in the workflow.

  package        Resolve the package target behind //tools/ci:editor_wasm_package and its one
                 output directory, copy that directory (dereferencing links) to
                 $RUNNER_TEMP/editor-wasm-package-geode, and write `package_target` and
                 `handoff_dir`.
  candidate      Stage $RUNNER_TEMP/donner-editor-wasm-geode-$GITHUB_SHA from the handoff: the
                 checked package under site/, the served files with a gzip-encoded editor.wasm
                 under deploy/, the proved MODULE.bazel.lock, provenance.json and a SHA256SUMS
                 manifest of site/ and deploy/. Needs PACKAGE_TARGET, GITHUB_RUN_ID and
                 GITHUB_RUN_ATTEMPT; writes `candidate_dir`.
  record-digest  Append the candidate's source revision, artifact digest (ARTIFACT_DIGEST) and
                 generated lockfile digest to the job summary.

A failing command ends the step with that command's exit status; any other failure exits 1.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
from collections.abc import Callable, Mapping

PACKAGE_ALIAS = "//tools/ci:editor_wasm_package"
# The files every package serves besides the catalog fonts that catalog-fonts.json lists.
SITE_FILES = (
    "CatalogFontNotices.txt",
    "browser-file-dialogs.js",
    "catalog-fonts.js",
    "catalog-fonts.json",
    "donner_icon.svg",
    "editor-bootstrap.js",
    "editor.css",
    "editor.js",
    "editor.wasm",
    "enable-threads.js",
    "index.html",
)
# The served files copied unchanged to deploy/; editor.wasm is served gzip-encoded instead.
DEPLOY_FILES = tuple(name for name in SITE_FILES if name != "editor.wasm")
MAX_DEPLOYED_WASM_GZIP_BYTES = 20 * 1024 * 1024
NPM_LOCKFILE = "donner/editor/wasm/tests/package-lock.json"
LABEL = re.compile(r"//[a-zA-Z0-9_./-]+:[a-zA-Z0-9_./-]+")


class StageError(Exception):
    """A failed check or command; `status` is the step's exit status."""

    def __init__(self, message: str, status: int = 1):
        super().__init__(message)
        self.status = status


def _run(command: list[str]) -> str:
    """Runs `command` and returns its stdout, without the trailing newlines (as `$(...)` does)."""
    result = subprocess.run(command, stdout=subprocess.PIPE, text=True, check=False)
    if result.returncode != 0:
        raise StageError("%s failed with exit status %d" % (command[0], result.returncode),
                         result.returncode)
    return result.stdout.rstrip("\n")


def _check(command: list[str]) -> None:
    """Runs `command` with its output in the log; a failure ends the step with its status."""
    status = subprocess.run(command, check=False).returncode
    if status != 0:
        raise StageError("%s failed with exit status %d" % (" ".join(command), status), status)


def _write_output(environ: Mapping[str, str], name: str, value: str) -> None:
    with open(environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
        output.write("%s=%s\n" % (name, value))


def _umask() -> int:
    mask = os.umask(0)
    os.umask(mask)
    return mask


def _copy_file(source: Path, destination: Path) -> None:
    """`cp SOURCE DESTINATION`: a new file gets the source's permissions less the umask."""
    created = not destination.exists()
    shutil.copyfile(source, destination)
    if created:
        os.chmod(destination, stat.S_IMODE(os.stat(source).st_mode) & 0o777 & ~_umask())


def _copy_contents(source: Path, destination: Path, dereference: bool) -> None:
    """`cp -R SOURCE/. DESTINATION/` (`cp -RL` when `dereference`).

    Directories are merged into existing ones. Without `dereference` a symbolic link is copied
    as a link; with it, the link's target is copied.
    """
    for entry in sorted(os.listdir(source)):
        _copy_entry(source / entry, destination / entry, dereference)


def _copy_entry(source: Path, target: Path, dereference: bool) -> None:
    if source.is_symlink() and not dereference:
        os.symlink(os.readlink(source), target)
    elif source.is_dir():
        created = not target.is_dir()
        if created:
            target.mkdir()
        _copy_contents(source, target, dereference)
        # Like cp, set a new directory's permissions after filling it, so a read-only source
        # directory does not block its own copy.
        if created:
            os.chmod(target, stat.S_IMODE(os.stat(source).st_mode) & 0o777 & ~_umask())
    elif source.is_file():
        _copy_file(source, target)
    else:
        raise StageError("cannot copy %s: not a regular file or directory" % source)


def _copy_tree(source: Path, destination: Path) -> None:
    """`cp -R SOURCE DESTINATION` for a directory: into DESTINATION/SOURCE if it exists."""
    if destination.is_dir():
        destination = destination / source.name
    destination.mkdir()
    _copy_contents(source, destination, dereference=False)
    os.chmod(destination, stat.S_IMODE(os.stat(source).st_mode) & 0o777 & ~_umask())


def _regular_files(root: Path, *directories: str) -> list[str]:
    """`find DIRECTORIES -type f` under `root`, as root-relative paths in byte order."""
    found = []
    for directory in directories:
        for parent, _, names in os.walk(root / directory):
            for name in names:
                path = Path(parent) / name
                if stat.S_ISREG(os.lstat(path).st_mode):
                    found.append(path.relative_to(root).as_posix())
    return sorted(found, key=os.fsencode)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _source_arguments(environ: Mapping[str, str]) -> list[str]:
    if "GITHUB_SHA" not in environ:
        return []
    commit = environ["GITHUB_SHA"]
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise StageError("GITHUB_SHA must be a full lowercase source commit")
    return ["--//build_defs:release_source_commit=" + commit]


def stage_package(environ: Mapping[str, str], run: Callable[[list[str]], str] = _run) -> None:
    source_args = _source_arguments(environ)
    package_target = run(["bazelisk", "query", "--output=label",
                          "labels(actual, %s)" % PACKAGE_ALIAS])
    if not LABEL.fullmatch(package_target):
        raise StageError("%s resolves to an unexpected label: %r" % (PACKAGE_ALIAS, package_target))
    _write_output(environ, "package_target", package_target)
    package_path = run(["bazelisk", "cquery", "--config=editor-wasm", *source_args, "--output=files",
                        PACKAGE_ALIAS])
    if not package_path:
        raise StageError("%s has no output" % PACKAGE_ALIAS)
    if "\n" in package_path:
        raise StageError("%s has more than one output:\n%s" % (PACKAGE_ALIAS, package_path))
    if not os.path.isdir(package_path):
        raise StageError("%s output is not a directory: %s" % (PACKAGE_ALIAS, package_path))
    package_dir = Path(os.path.realpath(package_path))
    runner_temp = environ["RUNNER_TEMP"]
    handoff_dir = f"{runner_temp}/editor-wasm-package-geode"
    Path(handoff_dir).mkdir(parents=True, exist_ok=True)
    _copy_contents(package_dir, Path(handoff_dir), dereference=True)
    _write_output(environ, "handoff_dir", handoff_dir)


def _expected_site_files(site_dir: Path) -> list[str]:
    manifest = json.loads((site_dir / "catalog-fonts.json").read_text(encoding="utf-8"))
    fonts = "\n".join(font["path"] for font in manifest["fonts"]) + "\n"
    return sorted([*SITE_FILES, *fonts.split("\n")[:-1]], key=os.fsencode)


def _provenance(environ: Mapping[str, str], lockfile_sha256: str,
                generated_lock_sha256: str) -> str:
    # Written as text, field by field, so its bytes stay those the release tooling has always
    # received; the package label was validated when the handoff was staged.
    return ('{"source_revision":"%s","producer_run_id":"%s","producer_attempt":"%s",'
            '"lockfile_sha256":"%s","generated_lock_sha256":"%s",'
            '"bazel_configs":["editor-wasm"],"targets":["%s"],'
            '"renderer":"geode","transport_encoding":"gzip"}\n' % (
                environ["GITHUB_SHA"], environ["GITHUB_RUN_ID"], environ["GITHUB_RUN_ATTEMPT"],
                lockfile_sha256, generated_lock_sha256, environ["PACKAGE_TARGET"]))


def _checksum_manifest(candidate_dir: Path) -> str:
    """SHA256SUMS lines (`shasum -a 256` format) for every file of site/ and deploy/."""
    lines = []
    for path in _regular_files(candidate_dir, "site", "deploy"):
        if "\n" in path or "\\" in path:
            raise StageError("unsupported character in candidate path %r" % path)
        lines.append("%s  %s\n" % (_sha256(candidate_dir / path), path))
    return "".join(lines)


def stage_candidate(environ: Mapping[str, str]) -> None:
    missing = [name for name in ("RUNNER_TEMP", "GITHUB_SHA", "GITHUB_RUN_ID",
                                 "GITHUB_RUN_ATTEMPT", "PACKAGE_TARGET", "GITHUB_OUTPUT")
               if name not in environ]
    if missing:
        raise StageError("missing environment: %s" % " ".join(missing))
    _source_arguments(environ)
    runner_temp = environ["RUNNER_TEMP"]
    package_dir = Path(f"{runner_temp}/editor-wasm-package-geode")
    candidate_dir = f"{runner_temp}/donner-editor-wasm-geode-{environ['GITHUB_SHA']}"
    site_dir = Path(candidate_dir) / "site"
    deploy_dir = Path(candidate_dir) / "deploy"
    site_dir.mkdir(parents=True, exist_ok=True)
    deploy_dir.mkdir(parents=True, exist_ok=True)
    _copy_contents(package_dir, site_dir, dereference=False)

    _check(["python3", "donner/editor/wasm/catalog_package_integrity_test.py",
            "--package", str(site_dir)])
    expected = "\n".join(_expected_site_files(site_dir))
    actual = "\n".join(_regular_files(site_dir, "."))
    if actual != expected:
        raise StageError("Package file set mismatch. Expected:\n%s\nActual:\n%s" % (
            expected, actual))

    for name in DEPLOY_FILES:
        _copy_file(site_dir / name, deploy_dir / name)
    _copy_tree(site_dir / "fonts", deploy_dir / "fonts")
    compressed = deploy_dir / "editor.wasm.gz"
    with open(compressed, "wb") as stream:
        status = subprocess.run(["gzip", "-n", "-9", "-c", str(site_dir / "editor.wasm")],
                                stdout=stream, check=False).returncode
    if status != 0:
        raise StageError("gzip failed with exit status %d" % status, status)
    size = compressed.stat().st_size
    if size > MAX_DEPLOYED_WASM_GZIP_BYTES:
        raise StageError("editor.wasm.gz is %d bytes, over the %d byte limit" % (
            size, MAX_DEPLOYED_WASM_GZIP_BYTES))

    # lockfile_sha256 is the browser harness npm lock; generated_lock_sha256 is the
    # MODULE.bazel.lock proved by the package build and retained beside the package.
    module_lock = Path(candidate_dir) / "MODULE.bazel.lock"
    _copy_file(Path(f"{runner_temp}/editor-wasm-module.lock"), module_lock)
    lockfile_sha256 = _sha256(Path(NPM_LOCKFILE))
    generated_lock_sha256 = _sha256(module_lock)
    (Path(candidate_dir) / "provenance.json").write_bytes(
        _provenance(environ, lockfile_sha256, generated_lock_sha256).encode(
            "utf-8", "surrogateescape"))

    (Path(candidate_dir) / "SHA256SUMS").write_bytes(
        _checksum_manifest(Path(candidate_dir)).encode("utf-8", "surrogateescape"))
    _write_output(environ, "candidate_dir", candidate_dir)


def record_digest(environ: Mapping[str, str]) -> None:
    lock = Path(environ["RUNNER_TEMP"]) / "editor-wasm-module.lock"
    try:
        generated_lock_sha256 = _sha256(lock)
    except OSError as error:
        # The summary is informational; staging the candidate already required this lock.
        print("record-digest: %s" % error, file=sys.stderr)
        generated_lock_sha256 = ""
    with open(environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
        summary.write("### Geode deployment candidate\n"
                      "- Source revision: `%s`\n"
                      "- Artifact digest: `%s`\n"
                      "- Generated MODULE.bazel.lock SHA-256: `%s`\n" % (
                          environ.get("GITHUB_SHA", ""), environ.get("ARTIFACT_DIGEST", ""),
                          generated_lock_sha256))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("package", "candidate", "record-digest"))
    arguments = parser.parse_args(argv)
    os.chdir(Path(os.path.realpath(os.path.dirname(os.path.abspath(__file__)))).parent.parent)
    try:
        if arguments.command == "package":
            stage_package(os.environ)
        elif arguments.command == "candidate":
            stage_candidate(os.environ)
        else:
            record_digest(os.environ)
    except StageError as error:
        print("editor_wasm_stage.py %s: %s" % (arguments.command, error), file=sys.stderr)
        return error.status
    except (OSError, KeyError, ValueError) as error:
        print("editor_wasm_stage.py %s: %s: %s" % (
            arguments.command, type(error).__name__, error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
