#!/usr/bin/env python3
"""The BCR Preflight workflow's step commands, one subcommand per step.

Run from the repository root (the workflow's checkout) as `python3 -B -m tools.bcr_preflight
COMMAND`, so CI and a maintainer reproducing a step run the same commands. Each subcommand runs
its commands in order as phases: a phase is a collapsible `::group::` in the GitHub Actions log
and a row of the job summary when $GITHUB_STEP_SUMMARY is set, and the first failing command ends
the step with its exit status. Paths follow the workflow's layout under --temp (default
$RUNNER_TEMP; see Layout), which the workflow's upload, download and attestation steps name
directly. The commit, run ID and attempt default to $SOURCE_COMMIT, $GITHUB_RUN_ID and
$GITHUB_RUN_ATTEMPT. Permissions, attestation and artifact transfer stay in the workflow.

  scope           `full_matrix=true|false` to $GITHUB_OUTPUT. A push runs the slim check; a pull
                  request runs the full matrix when its diff since the merge base touches a
                  release or packaging input (RELEASE_PATHS) or cannot be listed; schedule and
                  manual dispatch run the full matrix.
  admission --upstream DIR
                  Install the upstream BCR validator's hash-locked requirements into a private
                  virtual environment and validate the candidate entry with tools.bcr_admission.
  consumer        Resolve the candidate archive through the disposable registry from the example
                  consumer module, test it, build the four Donner libraries, then audit their
                  configured closure with tools/bcr_dependency_closure.py.
  cli --platform PLATFORM
                  Build the release CLI, its NOTICE and the compiler identity probe twice (resolve
                  the lockfile, then prove the build under it), test NOTICE coverage, and package
                  the CLI with tools.release_cli.
  verify [--with-source]
                  Verify both CLI artifacts of this attempt with tools.release_cli, and with
                  --with-source the preflight source archive with tools.bcr_source first.
  explain-missing-cli
                  Explain a failed CLI artifact download and fail.
"""

from __future__ import annotations

import argparse
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
import fnmatch
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

# Changed paths that select the full preflight matrix on a pull request: release and packaging
# inputs, this tool and the workflows it serves. Matched as shell patterns, where `*` also
# matches `/`.
RELEASE_PATHS = (
    "MODULE.bazel", "MODULE.bazel.lock", ".bazelversion", ".bazelrc", ".bcr/*",
    "examples/bazel_consumer/*", "third_party/licenses/*", "third_party/bazel/*",
    "third_party/BUILD*", "third_party/*.BUILD", "third_party/*.bzl",
    "third_party/patches/*", "build_defs/licenses.bzl", "build_defs/dep_audit.bzl",
    "tools/BUILD.bazel", "tools/bcr_*", "tools/release_*", "tools/notice*",
    "tools/render_notice*", "tools/default_notice*", "tools/ci/BUILD.bazel",
    "tools/ci/release_*", ".github/workflows/bcr_preflight.yml",
    ".github/workflows/publish_bcr.yml", ".github/workflows/release.yml",
)

# The release CLI, its NOTICE and the compiler identity probe, built in one invocation so the
# NOTICE and the provenance's compiler describe the CLI's own configuration.
RELEASE_TARGETS = ("//tools/ci:release_svg", "//tools/ci:release_svg_notice",
                   "//tools/ci:release_compiler_identity")
# The NOTICE must attribute every third-party component in the CLI configuration's closure.
NOTICE_COVERAGE = "//tools/ci:release_svg_notice_coverage"
# The Donner libraries a consumer builds, whose configured closure the consumer audit checks.
CONSUMER_LIBRARIES = ("@donner//donner/base:base", "@donner//donner/css:css",
                      "@donner//donner/svg:svg", "@donner//donner/svg/renderer:renderer")
PUBLIC_REGISTRY = "https://bcr.bazel.build"
# Release platform to the short name of its CLI directories.
PLATFORMS = {"linux-x86-64": "linux", "darwin-arm64": "macos"}

MISSING_CLI_RERUN = (
    "If the download error above is 'Artifact not found', this attempt reran only failed jobs, "
    "and the CLI artifacts are bound to the attempt that built them; rerun all jobs.")
MISSING_CLI_OTHER = (
    "Otherwise the download failed for another reason; see the error above and the GitHub "
    "status page before rerunning.")


@dataclass(frozen=True)
class Layout:
    """The workflow's directories under the runner's temporary directory."""

    temp: str

    @property
    def bcr(self) -> str:
        return f"{self.temp}/donner-bcr"

    @property
    def artifacts(self) -> str:
        """The source archive: written by prepare, downloaded by consumer and qualify."""
        return f"{self.bcr}/artifacts"

    @property
    def registry(self) -> str:
        """The disposable registry and generated entry prepare validates and retains."""
        return f"{self.bcr}/registry"

    @property
    def admission_report(self) -> str:
        return f"{self.bcr}/admission.json"

    @property
    def validator(self) -> str:
        """The upstream validator's virtual environment, outside every retained directory."""
        return f"{self.temp}/bcr-validator"

    @property
    def entry_registry(self) -> str:
        """The retained registry, as the consumer job downloads it under `entry`."""
        return f"{self.bcr}/entry/registry"

    @property
    def consumer_test(self) -> str:
        return f"{self.bcr}/consumer-test"

    @property
    def closure_labels(self) -> str:
        return f"{self.bcr}/consumer-closure.labels"

    @property
    def source(self) -> str:
        """The source archive as the attestation job downloads it."""
        return f"{self.bcr}/source"

    def cli_output(self, platform: str) -> str:
        """Where a CLI build job packages the CLI it uploads."""
        return f"{self.temp}/donner-cli-{PLATFORMS[platform]}"

    def cli_download(self, platform: str) -> str:
        """Where the attestation and qualification jobs download a CLI artifact."""
        return f"{self.bcr}/cli-{PLATFORMS[platform]}"


@dataclass(frozen=True)
class Command:
    """One phase: a command, where it runs, and how its output is checked.

    `stdout` names a file the command's output replaces, as a shell `>` would. `expect_stdout`
    requires the command's output, without trailing newlines, to equal that value.
    """

    phase: str
    argv: tuple[str, ...]
    cwd: str | None = None
    stdout: str | None = None
    expect_stdout: str | None = None


def is_release_path(path: str) -> bool:
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in RELEASE_PATHS)


def _git_output(args: Sequence[str]) -> str | None:
    # Paths are bytes to Git. Bytes that are not UTF-8 decode to surrogate escapes, so such a
    # path is still classified (as the shell `case` this replaced matched raw bytes) and prints
    # back unchanged.
    result = subprocess.run(["git", *args], stdout=subprocess.PIPE, encoding="utf-8",
                            errors="surrogateescape", check=False)
    return result.stdout.rstrip("\n") if result.returncode == 0 else None


def _print_raw(text: str) -> None:
    """Prints text that may hold surrogate-escaped path bytes, writing those bytes as they were."""
    sys.stdout.flush()
    sys.stdout.buffer.write(text.encode("utf-8", "surrogateescape") + b"\n")
    sys.stdout.buffer.flush()


def _pull_request_shas(environ: Mapping[str, str]) -> tuple[str, str]:
    """The pull request's base and head SHAs from the event payload; empty when unreadable."""
    try:
        event = json.loads(Path(environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
        pull_request = event["pull_request"]
        return str(pull_request["base"]["sha"] or ""), str(pull_request["head"]["sha"] or "")
    except (KeyError, OSError, TypeError, ValueError):
        return "", ""


def full_matrix(environ: Mapping[str, str]) -> bool:
    """Whether this run qualifies a release candidate with the full matrix."""
    event_name = environ.get("GITHUB_EVENT_NAME", "")
    if event_name == "push":
        return False
    if event_name != "pull_request":
        return True
    base_sha, head_sha = _pull_request_shas(environ)
    diff_base = _git_output(["merge-base", base_sha, head_sha])
    changed = None
    if diff_base is not None:
        changed = _git_output(["-c", "core.quotePath=false", "diff", "--no-renames",
                               "--name-only", diff_base, head_sha])
    if not changed:
        print("Could not list the pull request's changes; selecting the full matrix.")
        return True
    for path in changed.split("\n"):
        if is_release_path(path):
            _print_raw(f"Full preflight matrix selected by {path}")
            return True
    return False


def admission_plan(layout: Layout, upstream: str, commit: str) -> list[Command]:
    venv = layout.validator
    return [
        Command("Create the validator environment", ("python3", "-m", "venv", venv)),
        Command("Install the validator's hash-locked requirements",
                (f"{venv}/bin/pip", "install", "--require-hashes",
                 "-r", f"{upstream}/tools/requirements_lock.txt")),
        Command("Validate registry admission",
                (f"{venv}/bin/python", "-B", "-m", "tools.bcr_admission",
                 "--upstream", upstream, "--registry", layout.registry,
                 "--artifacts", layout.artifacts, "--commit", commit,
                 "--report", layout.admission_report)),
    ]


def consumer_registry_flags(layout: Layout) -> tuple[str, ...]:
    return ("--lockfile_mode=off", f"--registry=file://{layout.consumer_test}/registry",
            f"--registry={PUBLIC_REGISTRY}")


def consumer_plan(layout: Layout, workspace: str, commit: str) -> list[Command]:
    """Exercise the archive through registry resolution, then audit the libraries' closure.

    The audit reads the configured closure of the Donner libraries built here, not the consumer
    test, whose GoogleTest/pixelmatch dev deps must not mask Donner's production dependency
    boundary. It rejects development-only, Geode, WebGPU and test targets and any Rust rule,
    source, toolchain or crate, requires a cc_toolchain rule and a C++ exec tool outside the
    libraries' configuration, then requires Rust rows appended to the query output to be
    rejected. It is the last command, so its status is the step's.
    """
    consumer = f"{layout.consumer_test}/consumer"
    flags = consumer_registry_flags(layout)
    return [
        Command("Generate the consumer workspace from the archive",
                ("python3", "-B", "-m", "tools.bcr_source", "consumer",
                 "--artifacts", layout.artifacts, "--registry", layout.entry_registry,
                 "--output", layout.consumer_test, "--commit", commit)),
        Command("Test the consumer", ("bazelisk", "test", *flags, "//:all"), cwd=consumer),
        Command("Build the Donner libraries",
                ("bazelisk", "build", *flags, *CONSUMER_LIBRARIES), cwd=consumer),
        Command("Confirm the audit tools come from the source commit",
                ("git", "-C", workspace, "rev-parse", "HEAD"), cwd=consumer,
                expect_stdout=commit),
        Command("Query the libraries' configured closure",
                ("bazelisk", "cquery", *flags, "deps(%s)" % " + ".join(CONSUMER_LIBRARIES),
                 "--output=label_kind"), cwd=consumer, stdout=layout.closure_labels),
        Command("Audit the configured closure",
                ("python3", "-B", f"{workspace}/tools/bcr_dependency_closure.py",
                 "--module", f"{workspace}/MODULE.bazel", "--labels", layout.closure_labels),
                cwd=consumer),
    ]


def cli_plan(layout: Layout, platform: str, commit: str, run_id: str,
             attempt: str) -> list[Command]:
    """Build the release CLI and bind it to the preflight candidate.

    The repository ignores MODULE.bazel.lock: resolve once, then prove the build is complete
    under that generated, retained lockfile.
    """
    return [
        Command("Resolve the module graph",
                ("bazelisk", "build", "--lockfile_mode=update", "-c", "opt", *RELEASE_TARGETS)),
        Command("Build under the resolved lockfile",
                ("bazelisk", "build", "--lockfile_mode=error", "-c", "opt", *RELEASE_TARGETS)),
        Command("Test NOTICE coverage",
                ("bazelisk", "test", "--lockfile_mode=error", "-c", "opt", NOTICE_COVERAGE)),
        Command("Package the CLI",
                ("python3", "-B", "-m", "tools.release_cli", "package",
                 "--platform", platform, "--binary", "bazel-bin/donner/svg/tool/donner-svg",
                 "--notice", "bazel-bin/third_party/licenses/notice_cli.txt",
                 "--compiler-probe", "bazel-bin/tools/ci/release_compiler_identity",
                 "--output", layout.cli_output(platform), "--commit", commit,
                 "--run-id", run_id, "--attempt", attempt)),
    ]


def verify_plan(layout: Layout, commit: str, run_id: str, attempt: str,
                with_source: bool) -> list[Command]:
    commands = []
    if with_source:
        commands.append(Command("Verify the source archive",
                                ("python3", "-B", "-m", "tools.bcr_source", "verify",
                                 "--artifacts", layout.source, "--commit", commit)))
    for platform in PLATFORMS:
        commands.append(Command(f"Verify the {platform} CLI",
                                ("python3", "-B", "-m", "tools.release_cli", "verify",
                                 "--artifacts", layout.cli_download(platform),
                                 "--platform", platform, "--commit", commit,
                                 "--run-id", run_id, "--attempt", attempt)))
    return commands


def _summary(environ: Mapping[str, str], *lines: str) -> None:
    path = environ.get("GITHUB_STEP_SUMMARY")
    if path:
        with open(path, "a", encoding="utf-8") as summary:
            summary.write("".join(line + "\n" for line in lines))


def _execute(command: Command) -> int:
    shown = shlex.join(command.argv)
    if command.stdout:
        shown += " > " + shlex.quote(command.stdout)
    if command.cwd:
        shown = "(cd %s && %s)" % (shlex.quote(command.cwd), shown)
    print("+ " + shown, flush=True)
    # As in a shell: a missing directory or unwritable output fails with status 1, a missing
    # program with 127.
    if command.cwd and not os.path.isdir(command.cwd):
        print(f"cd: {command.cwd}: No such directory", file=sys.stderr, flush=True)
        return 1
    try:
        output = open(command.stdout, "w", encoding="utf-8") if command.stdout else None
    except OSError as error:
        print(f"{command.stdout}: {error.strerror}", file=sys.stderr, flush=True)
        return 1
    capture = subprocess.PIPE if command.expect_stdout is not None else output
    try:
        result = subprocess.run(command.argv, cwd=command.cwd, stdout=capture, text=True,
                                check=False)
    except FileNotFoundError:
        print(f"{command.argv[0]}: command not found", file=sys.stderr, flush=True)
        return 127
    finally:
        if output:
            output.close()
    status = 128 - result.returncode if result.returncode < 0 else result.returncode
    if command.expect_stdout is None:
        return status
    actual = result.stdout.rstrip("\n")
    print(actual, flush=True)
    if status != 0 or actual != command.expect_stdout:
        print(f"Expected {command.expect_stdout!r}, got {actual!r}", file=sys.stderr, flush=True)
        return 1
    return 0


def run_lane(title: str, commands: Sequence[Command],
             environ: Mapping[str, str] = os.environ) -> int:
    """Runs each command as a phase; the first failure ends the lane with its status."""
    _summary(environ, f"### {title}", "", "| Phase | Result |", "| --- | --- |")
    for command in commands:
        print(f"::group::{command.phase}", flush=True)
        status = _execute(command)
        print("::endgroup::", flush=True)
        if status != 0:
            _summary(environ, f"| {command.phase} | failed (exit {status}) |")
            print(f"::error::{command.phase} failed with exit status {status}", flush=True)
            return status
        _summary(environ, f"| {command.phase} | passed |")
    return 0


def explain_missing_cli(environ: Mapping[str, str]) -> int:
    try:
        rerun = int(environ.get("GITHUB_RUN_ATTEMPT", "")) > 1
    except ValueError:
        rerun = False
    if rerun:
        print(f"::error::{MISSING_CLI_RERUN}")
    print(f"::error::{MISSING_CLI_OTHER}")
    return 1


def main(argv: Sequence[str] | None = None, environ: Mapping[str, str] = os.environ) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--temp", default=environ.get("RUNNER_TEMP"),
                        help="the runner's temporary directory (default $RUNNER_TEMP)")
    parser.add_argument("--commit", default=environ.get("SOURCE_COMMIT"),
                        help="the preflight source commit (default $SOURCE_COMMIT)")
    parser.add_argument("--run-id", default=environ.get("GITHUB_RUN_ID"))
    parser.add_argument("--attempt", default=environ.get("GITHUB_RUN_ATTEMPT"))
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("scope")
    admission = commands.add_parser("admission")
    admission.add_argument("--upstream", required=True,
                           help="checkout of the upstream Bazel Central Registry")
    commands.add_parser("consumer")
    cli = commands.add_parser("cli")
    cli.add_argument("--platform", required=True, choices=sorted(PLATFORMS))
    verify = commands.add_parser("verify")
    verify.add_argument("--with-source", action="store_true")
    commands.add_parser("explain-missing-cli")
    args = parser.parse_args(argv)

    if args.command == "scope":
        value = "true" if full_matrix(environ) else "false"
        with open(environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
            output.write(f"full_matrix={value}\n")
        return 0
    if args.command == "explain-missing-cli":
        return explain_missing_cli(environ)

    needed = {"temp": "--temp or $RUNNER_TEMP", "commit": "--commit or $SOURCE_COMMIT"}
    if args.command in ("cli", "verify"):
        needed.update({"run_id": "--run-id or $GITHUB_RUN_ID",
                       "attempt": "--attempt or $GITHUB_RUN_ATTEMPT"})
    missing = [flag for name, flag in needed.items() if not getattr(args, name)]
    if missing:
        parser.error("%s needs %s" % (args.command, ", ".join(missing)))
    layout = Layout(args.temp)
    if args.command == "admission":
        return run_lane("BCR registry admission",
                        admission_plan(layout, args.upstream, args.commit), environ)
    if args.command == "consumer":
        workspace = environ.get("GITHUB_WORKSPACE") or os.getcwd()
        return run_lane("BCR consumer", consumer_plan(layout, workspace, args.commit), environ)
    if args.command == "cli":
        return run_lane(f"Release CLI ({args.platform})",
                        cli_plan(layout, args.platform, args.commit, args.run_id, args.attempt),
                        environ)
    return run_lane("Preflight artifact verification",
                    verify_plan(layout, args.commit, args.run_id, args.attempt,
                                args.with_source), environ)


if __name__ == "__main__":
    sys.exit(main())
