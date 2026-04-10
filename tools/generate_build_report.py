from __future__ import annotations

import argparse
import json
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
    # Bazel canonical repo labels take several shapes:
    #   @@zlib+//:foo                          (BCR direct module)
    #   @@+_repo_rules+entt//src:entt          (local_repository via extension)
    #   @@non_bcr_deps+harfbuzz//:harfbuzz     (module extension override)
    # Strip a trailing "+" and take the last non-empty "+"-delimited
    # component so we end up with the human-readable repo name in each case.
    # Previously the normalizer dropped BCR-canonical labels like
    # `@zlib+//:...` entirely because `"zlib+".split("+")[-1]` is empty.
    repo = repo.rstrip("+")
    if "+" in repo:
        parts = [part for part in repo.split("+") if part]
        repo = parts[-1] if parts else ""
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


@dataclass(frozen=True)
class LicenseEntry:
    package_name: str
    license_kinds: tuple[str, ...]
    license_url: str  # Markdown-ready link target (may be empty).


# Fallback upstream URLs for packages whose BCR `license()` overlays do not
# set `package_url`. Keyed by the lowercase package name we write into the
# manifest (which is either the explicit `package_name` or the repo name
# derived by `_fallback_package_name` in `build_defs/licenses.bzl`).
_PACKAGE_URL_FALLBACKS: dict[str, str] = {
    "libpng": "http://www.libpng.org/pub/png/libpng.html",
    "zlib": "https://zlib.net/",
    "skia": "https://skia.org/",
}


# Override the `license_kinds` reported for packages whose upstream overlay
# uses the generic `@rules_license//licenses/generic:notice` placeholder
# instead of a specific SPDX identifier. The NOTICE.txt still carries the
# verbatim license text from upstream; this only affects the display in the
# build report.
_PACKAGE_LICENSE_KIND_OVERRIDES: dict[str, tuple[str, ...]] = {
    "skia": ("BSD-3-Clause",),
}


def load_license_manifest(
    runner: CommandRunner,
    notice_target: str,
    *,
    bazel_bin: Path,
) -> tuple[dict[str, LicenseEntry], CommandResult]:
    """Build the given `donner_notice_file` target and parse its JSON manifest.

    Returns a `{repo_name: LicenseEntry}` dict keyed by a normalized repo name
    (matching what `_normalize_external_dependency_repo` produces) so the
    external-dependencies section can look up license info per dep.
    """
    args = ["bazel", "build", notice_target]
    result = runner.run(f"licenses:{notice_target}", args)
    if not result.success:
        return {}, result

    # Resolve the manifest path: `//third_party/licenses:notice_default`
    # → `bazel-bin/third_party/licenses/notice_default.json`.
    if not notice_target.startswith("//"):
        return {}, result
    package, _, name = notice_target[2:].partition(":")
    manifest_path = bazel_bin / package / f"{name}.json"
    if not manifest_path.exists():
        return {}, result

    try:
        data = json.loads(manifest_path.read_text())
    except (OSError, json.JSONDecodeError):
        return {}, result

    out: dict[str, LicenseEntry] = {}
    for raw in data.get("licenses", []):
        package_name = raw.get("package_name") or ""
        kinds = tuple(raw.get("license_kinds") or [])
        override_kinds = _PACKAGE_LICENSE_KIND_OVERRIDES.get(package_name.lower())
        if override_kinds is not None:
            kinds = override_kinds
        license_url = raw.get("package_url") or _PACKAGE_URL_FALLBACKS.get(
            package_name.lower(), ""
        )

        # Keys the dep list might use to look this entry up. The build
        # report's dependency normalizer yields a repo name (e.g. "entt",
        # "libpng"), so register both the package_name and several label-
        # derived candidates to maximize hit rate.
        candidates: set[str] = set()
        if package_name:
            candidates.add(package_name.lower())
        label = raw.get("label") or ""
        if label.startswith("@@"):
            repo = label[2:].split("//", 1)[0].rstrip("+")
            if repo:
                candidates.add(repo.lower())
        # license_text short_path often encodes the repo directory, e.g.
        # "../libpng+/LICENSE" or "../+_repo_rules+entt/LICENSE".
        short = raw.get("license_text") or ""
        if short.startswith("../"):
            first = short[3:].split("/", 1)[0]
            normalized = _normalize_external_dependency_repo("@@" + first + "//:_")
            if normalized:
                candidates.add(normalized.lower())

        entry = LicenseEntry(
            package_name=package_name,
            license_kinds=kinds,
            license_url=license_url,
        )
        for key in candidates:
            out[key] = entry

    return out, result


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
    result = runner.run("lines-of-code", args)

    # If `cloc` isn't installed, CommandRunner returns exit 127 via
    # FileNotFoundError. Surface a clearer hint in that case, but always
    # route through the injected runner so callers (and tests) can mock
    # the section deterministically.
    failure_hint = None
    if not result.success and shutil.which("cloc") is None:
        failure_hint = "(cloc not available — install `cloc` to populate this section)"

    return SectionResult(
        "Lines of Code",
        _result_status(result),
        result.duration_sec,
        _render_command_block(
            format_command(args),
            result,
            empty_output_message="(cloc produced no output)",
            failure_hint=failure_hint,
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


@dataclass(frozen=True)
class DependencyVariant:
    category_name: str
    configs: tuple[str, ...]
    notice_target: str


_DEPENDENCY_VARIANTS: tuple[DependencyVariant, ...] = (
    DependencyVariant(
        category_name="Default (tiny-skia)",
        configs=(),
        notice_target="//third_party/licenses:notice_default",
    ),
    DependencyVariant(
        category_name="tiny-skia + text-full",
        configs=("--config=text-full",),
        notice_target="//third_party/licenses:notice_text_full",
    ),
    DependencyVariant(
        category_name="skia + text-full",
        configs=("--config=skia", "--config=text-full"),
        notice_target="//third_party/licenses:notice_skia_text_full",
    ),
)


def _resolve_bazel_bin() -> Path:
    try:
        out = subprocess.check_output(
            ["bazel", "info", "bazel-bin"], text=True
        ).strip()
        return Path(out)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return Path("bazel-bin")


def _format_dependency_line(dependency: str, entry: typing.Optional[LicenseEntry]) -> str:
    if entry is None:
        return f"- {dependency}"

    name = f"[{dependency}]({entry.license_url})" if entry.license_url else dependency
    kinds = ", ".join(entry.license_kinds)
    if kinds:
        return f"- {name} — {kinds}"
    return f"- {name}"


def make_external_dependencies_section(
    runner: CommandRunner,
    *,
    bazel_bin: typing.Optional[Path] = None,
) -> SectionResult:
    if bazel_bin is None:
        bazel_bin = _resolve_bazel_bin()

    status = "success"
    total_duration_sec = 0.0
    lines = []

    for variant in _DEPENDENCY_VARIANTS:
        command = [
            "bazel",
            "cquery",
            "deps(//examples:svg_to_png)",
        ] + list(variant.configs)
        command_display = format_command(command)
        dependencies, result = query_external_dependencies(runner, variant.configs)
        total_duration_sec += result.duration_sec

        license_lookup, license_result = load_license_manifest(
            runner, variant.notice_target, bazel_bin=bazel_bin
        )
        total_duration_sec += license_result.duration_sec
        if not license_result.success:
            status = "failed"

        lines.append(f"### {variant.category_name}")
        lines.append(f"Generated with: `{command_display}`")
        lines.append(
            f"Licenses aggregated from: `{variant.notice_target}` "
            "(embed the generated NOTICE.txt for attribution)."
        )
        lines.append("")

        if result.success:
            if dependencies:
                for dependency in dependencies:
                    entry = license_lookup.get(dependency.lower())
                    lines.append(_format_dependency_line(dependency, entry))
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
