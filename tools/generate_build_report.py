from __future__ import annotations

import argparse
import platform
import shlex
import shutil
import subprocess
import sys
import time
import typing
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class ReportOptions:
    all: bool = False
    binary_size: bool = False
    coverage: bool = False
    tests: bool = False
    public_targets: bool = False
    external_dependencies: bool = False


@dataclass(frozen=True)
class ReportMetadata:
    command_line: str
    platform: str
    git_revision: str
    git_status: str


@dataclass(frozen=True)
class CommandResult:
    label: str
    args: tuple[str, ...]
    returncode: int
    stdout: str
    stderr: str
    duration_sec: float

    @property
    def success(self) -> bool:
        return self.returncode == 0


@dataclass(frozen=True)
class SectionResult:
    title: str
    status: str
    duration_sec: float
    content: str


def format_command(args: typing.Sequence[str]) -> str:
    return " ".join(shlex.quote(arg) for arg in args)


class CommandRunner:
    def __init__(
        self,
        progress_interval_sec: float = 15.0,
        status_stream: typing.TextIO = sys.stderr,
    ) -> None:
        self.progress_interval_sec = progress_interval_sec
        self.status_stream = status_stream

    def run(self, label: str, args: typing.Sequence[str]) -> CommandResult:
        command_text = format_command(args)
        print(f"[{label}] Running: {command_text}", file=self.status_stream, flush=True)

        start = time.monotonic()
        try:
            process = subprocess.Popen(
                list(args),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
        except FileNotFoundError as exc:
            duration_sec = time.monotonic() - start
            return CommandResult(
                label=label,
                args=tuple(args),
                returncode=127,
                stdout="",
                stderr=str(exc),
                duration_sec=duration_sec,
            )
        except KeyboardInterrupt:
            duration_sec = time.monotonic() - start
            return CommandResult(
                label=label,
                args=tuple(args),
                returncode=130,
                stdout="",
                stderr="Interrupted by user",
                duration_sec=duration_sec,
            )

        try:
            if self.progress_interval_sec <= 0:
                stdout, stderr = process.communicate()
            else:
                while True:
                    try:
                        stdout, stderr = process.communicate(timeout=self.progress_interval_sec)
                        break
                    except subprocess.TimeoutExpired:
                        elapsed_sec = time.monotonic() - start
                        print(
                            f"[{label}] Still running ({elapsed_sec:.0f}s elapsed)...",
                            file=self.status_stream,
                            flush=True,
                        )
        except KeyboardInterrupt:
            process.kill()
            stdout, stderr = process.communicate()
            stderr = (stderr + "\nInterrupted by user").strip()
            returncode = 130
        else:
            returncode = process.returncode

        duration_sec = time.monotonic() - start
        print(
            f"[{label}] Completed in {duration_sec:.1f}s with exit code {returncode}",
            file=self.status_stream,
            flush=True,
        )

        return CommandResult(
            label=label,
            args=tuple(args),
            returncode=returncode,
            stdout=stdout.strip(),
            stderr=stderr.strip(),
            duration_sec=duration_sec,
        )


def _check_output_or_default(args: typing.Sequence[str], default: str) -> str:
    try:
        return subprocess.check_output(list(args), text=True).strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return default


def gather_report_metadata(command_line: str) -> ReportMetadata:
    platform_summary = f"{platform.system()} {platform.machine()}"
    git_revision = _check_output_or_default(["git", "rev-parse", "HEAD"], "unknown")
    git_status = _check_output_or_default(["git", "status", "--short"], "")

    return ReportMetadata(
        command_line=command_line,
        platform=platform_summary,
        git_revision=git_revision,
        git_status=git_status,
    )


def _normalize_external_dependency_repo(label: str) -> typing.Optional[str]:
    label = label.strip()
    if not label.startswith("@"):
        return None

    repo = label.split("//", 1)[0].lstrip("@")
    if "+" in repo:
        repo = repo.split("+")[-1]
    repo = repo.strip("+")
    if not repo:
        return None

    if repo.startswith(
        (
            "bazel",
            "rules_",
            "platforms",
            "skia_user_config",
            "local_config_",
            "apple_support",
            "xcode_",
            "llvm_toolchain",
        )
    ):
        return None

    if "cc_configure_extension" in repo or "xcode_configure_extension" in repo:
        return None

    return repo


def query_external_dependencies(
    runner: CommandRunner, configs: typing.Sequence[str]
) -> tuple[typing.List[str], CommandResult]:
    args = [
        "bazel",
        "cquery",
        "deps(//examples:svg_to_png)",
        "--output=starlark",
        "--starlark:expr=target.label",
    ] + list(configs)
    label = "external-deps" if not configs else f"external-deps:{','.join(configs)}"
    result = runner.run(label, args)
    if not result.success:
        return [], result

    dependencies = set()
    for line in result.stdout.splitlines():
        repo = _normalize_external_dependency_repo(line)
        if repo is not None:
            dependencies.add(repo)

    return sorted(dependencies), result


def _render_command_block(
    command_display: str,
    result: CommandResult,
    *,
    empty_output_message: str,
    failure_hint: typing.Optional[str] = None,
) -> str:
    lines = ["```", f"$ {command_display}"]

    if result.stdout:
        lines.append(result.stdout)
    elif result.success:
        lines.append(empty_output_message)

    if result.stderr and not result.success:
        if result.stdout:
            lines.extend(["", "[stderr]"])
        lines.append(result.stderr)
    elif not result.success and not result.stdout:
        lines.append("(command produced no output)")

    if failure_hint:
        lines.extend(["", failure_hint])

    lines.append("```")
    return "\n".join(lines)


def _render_static_command_block(command_display: str, message: str) -> str:
    return "\n".join(["```", f"$ {command_display}", message, "```"])


def _result_status(result: CommandResult) -> str:
    return "success" if result.success else "failed"


def _count_dirty_paths(git_status: str) -> int:
    return len([line for line in git_status.splitlines() if line.strip()])


def _render_summary(metadata: ReportMetadata, sections: typing.Sequence[SectionResult]) -> str:
    lines = ["## Summary", ""]
    lines.append(f"- Platform: {metadata.platform}")
    lines.append(
        f"- Git revision: [{metadata.git_revision}]"
        f"(https://github.com/jwmcglynn/donner/commit/{metadata.git_revision})"
    )
    dirty_count = _count_dirty_paths(metadata.git_status)
    if dirty_count:
        lines.append(f"- Working tree: dirty ({dirty_count} paths)")
    else:
        lines.append("- Working tree: clean")

    for section in sections:
        lines.append(f"- {section.title}: {section.status} ({section.duration_sec:.1f}s)")

    lines.append("")
    return "\n".join(lines)


def make_lines_of_code_section(runner: CommandRunner) -> SectionResult:
    args = ["tools/cloc.sh"]
    if shutil.which("cloc") is None:
        content = _render_static_command_block(
            format_command(args),
            "(cloc not available — install `cloc` to populate this section)",
        )
        return SectionResult("Lines of Code", "failed", 0.0, content)

    result = runner.run("lines-of-code", args)
    return SectionResult(
        "Lines of Code",
        _result_status(result),
        result.duration_sec,
        _render_command_block(
            format_command(args),
            result,
            empty_output_message="(cloc produced no output)",
        ),
    )


def _copy_binary_size_asset(save_assets_to: typing.Optional[Path]) -> typing.Optional[str]:
    if save_assets_to is None:
        return None

    source = Path("build-binary-size/binary_size_bargraph.svg")
    if not source.exists():
        return None

    save_assets_to.mkdir(parents=True, exist_ok=True)
    destination = save_assets_to / "binary_size_bargraph.svg"
    shutil.copyfile(source, destination)
    return "\n\n![Binary size bar graph](binary_size_bargraph.svg)"


def make_binary_size_section(
    runner: CommandRunner, save_assets_to: typing.Optional[Path]
) -> SectionResult:
    args = ["tools/binary_size.sh"]
    result = runner.run("binary-size", args)
    lines = [f"Generated with: `{format_command(args)}`", ""]
    if result.stdout:
        lines.append(result.stdout)
    elif result.success:
        lines.append("```")
        lines.append("(binary_size.sh produced no output)")
        lines.append("```")
    else:
        lines.append(
            _render_command_block(
                format_command(args),
                result,
                empty_output_message="(binary_size.sh produced no output)",
            )
        )

    content = "\n".join(lines)
    if result.success:
        asset_markdown = _copy_binary_size_asset(save_assets_to)
        if asset_markdown:
            content += asset_markdown

    return SectionResult("Binary Size", _result_status(result), result.duration_sec, content)


def make_code_coverage_section(runner: CommandRunner) -> SectionResult:
    args = ["tools/coverage.sh", "--quiet"]
    result = runner.run("code-coverage", args)

    failure_hint = None
    if not result.success:
        missing_tools = []
        if shutil.which("genhtml") is None:
            missing_tools.append("genhtml")
        if shutil.which("lcov") is None:
            missing_tools.append("lcov")
        if missing_tools:
            failure_hint = "Missing prerequisite tool(s): " + ", ".join(missing_tools)

    return SectionResult(
        "Code Coverage",
        _result_status(result),
        result.duration_sec,
        _render_command_block(
            format_command(args),
            result,
            empty_output_message="(coverage.sh produced no output)",
            failure_hint=failure_hint,
        ),
    )


def make_tests_section(runner: CommandRunner) -> SectionResult:
    args = ["bazel", "test", "//donner/..."]
    result = runner.run("tests", args)
    return SectionResult(
        "Tests",
        _result_status(result),
        result.duration_sec,
        _render_command_block(
            format_command(args),
            result,
            empty_output_message="(test command produced no output)",
        ),
    )


def make_public_targets_section(runner: CommandRunner) -> SectionResult:
    args = [
        "bazel",
        "query",
        "kind(library, set(//donner/... //:*)) intersect attr(visibility, public, //...)",
    ]
    result = runner.run("public-targets", args)
    return SectionResult(
        "Public Targets",
        _result_status(result),
        result.duration_sec,
        _render_command_block(
            format_command(args),
            result,
            empty_output_message="(bazel query produced no output)",
        ),
    )


def make_external_dependencies_section(runner: CommandRunner) -> SectionResult:
    category_specs = [
        ("Default (tiny-skia)", []),
        ("tiny-skia + text-full", ["--config=text-full"]),
        ("skia + text-full", ["--config=skia", "--config=text-full"]),
    ]

    status = "success"
    total_duration_sec = 0.0
    lines = []

    for category_name, configs in category_specs:
        command = [
            "bazel",
            "cquery",
            "deps(//examples:svg_to_png)",
        ] + list(configs)
        command_display = format_command(command)
        dependencies, result = query_external_dependencies(runner, configs)
        total_duration_sec += result.duration_sec

        lines.append(f"### {category_name}")
        lines.append(f"Generated with: `{command_display}`")
        lines.append("")

        if result.success:
            if dependencies:
                for dependency in dependencies:
                    lines.append(f"- {dependency}")
            else:
                lines.append("(no external dependencies found)")
        else:
            status = "failed"
            lines.append(
                _render_command_block(
                    command_display,
                    result,
                    empty_output_message="(cquery produced no output)",
                )
            )

        lines.append("")

    return SectionResult(
        "External Dependencies",
        status,
        total_duration_sec,
        "\n".join(lines).rstrip(),
    )


def create_build_report(
    options: ReportOptions,
    save_assets_to: typing.Optional[Path] = None,
    *,
    runner: typing.Optional[CommandRunner] = None,
    metadata: typing.Optional[ReportMetadata] = None,
    command_line: typing.Optional[str] = None,
) -> str:
    command_line = command_line or " ".join(sys.argv)
    metadata = metadata or gather_report_metadata(command_line)
    runner = runner or CommandRunner()

    sections = [make_lines_of_code_section(runner)]
    if options.all or options.binary_size:
        sections.append(make_binary_size_section(runner, save_assets_to))
    if options.all or options.coverage:
        sections.append(make_code_coverage_section(runner))
    if options.all or options.tests:
        sections.append(make_tests_section(runner))
    if options.all or options.public_targets:
        sections.append(make_public_targets_section(runner))
    if options.all or options.external_dependencies:
        sections.append(make_external_dependencies_section(runner))

    report_lines = ["# Donner Build Report", ""]
    report_lines.append(f"Generated with: {metadata.command_line}")
    report_lines.append("")
    report_lines.append(_render_summary(metadata, sections))

    if metadata.git_status:
        report_lines.append("## Local Changes")
        report_lines.append("```")
        report_lines.append(metadata.git_status)
        report_lines.append("```")
        report_lines.append("")

    for section in sections:
        report_lines.append(f"## {section.title}")
        report_lines.append(section.content)
        report_lines.append("")

    return "\n".join(report_lines).rstrip() + "\n"


def parse_args(argv: typing.Optional[typing.Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate a build report for the Donner Bazel workspace."
    )
    parser.add_argument("--save", type=str, help="Path to save the build report")
    parser.add_argument("--all", action="store_true", help="Generate a full build report")
    parser.add_argument(
        "--binary-size", action="store_true", help="Generate the binary size section"
    )
    parser.add_argument(
        "--coverage", action="store_true", help="Generate the code coverage section"
    )
    parser.add_argument("--tests", action="store_true", help="Generate the tests section")
    parser.add_argument(
        "--public-targets", action="store_true", help="Generate the public targets section"
    )
    parser.add_argument(
        "--external-dependencies",
        action="store_true",
        help="Generate the external dependencies section",
    )
    parser.add_argument(
        "--progress-interval-sec",
        type=float,
        default=15.0,
        help="Seconds between progress heartbeats for long-running commands",
    )
    return parser.parse_args(argv)


def main(argv: typing.Optional[typing.Sequence[str]] = None) -> int:
    args = parse_args(argv)
    options = ReportOptions(
        all=args.all,
        binary_size=args.binary_size,
        coverage=args.coverage,
        tests=args.tests,
        public_targets=args.public_targets,
        external_dependencies=args.external_dependencies,
    )

    command_line = " ".join(sys.argv if argv is None else [sys.argv[0], *argv])
    save_path = Path(args.save) if args.save else None
    save_assets_to = save_path.parent if save_path is not None else None

    report = create_build_report(
        options,
        save_assets_to=save_assets_to,
        runner=CommandRunner(progress_interval_sec=args.progress_interval_sec),
        command_line=command_line,
    )

    if save_path is not None:
        save_path.write_text(report)
        print(f"Saved build report to {save_path}")
    else:
        print(report)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
