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
import zipfile
from dataclasses import dataclass
from pathlib import Path


# Link modes control how the build report references coverage / binary-size
# HTML artifacts:
#
#   "docs"  — used when regenerating the checked-in ``docs/build_report.md``.
#             Binary-size links are relative (``reports/binary-size/...``) so
#             they render from GitHub's web view of the repo and from the
#             Doxygen site alike. Coverage is too large to ship unzipped
#             (26M / hundreds of files — noisy for fuzzy file search) so it
#             is packaged as ``docs/reports/coverage.zip`` and extracted by
#             ``tools/build_docs.sh`` during the GitHub Pages build; the
#             coverage link therefore points at the absolute docs-site URL
#             (the only place the extracted tree is actually browsable).
#
#   "local" — point at the raw generator output (``coverage-report/``,
#             ``build-binary-size/``). Useful for devs regenerating the report
#             locally without touching ``docs/``.
#
#   "site"  — absolute URLs to the published docs site for every artifact.
LINK_MODE_DOCS = "docs"
LINK_MODE_LOCAL = "local"
LINK_MODE_SITE = "site"
LINK_MODES = (LINK_MODE_DOCS, LINK_MODE_LOCAL, LINK_MODE_SITE)

# Published docs site root.
DOCS_SITE_BASE_URL = "https://jwmcglynn.github.io/donner"


@dataclass(frozen=True)
class LinkTargets:
    """Resolved URLs / paths the build-report sections should hyperlink to."""

    coverage_index: str
    binary_size_report: str
    binary_size_bargraph: str


def _resolve_link_targets(link_mode: str) -> LinkTargets:
    base = DOCS_SITE_BASE_URL.rstrip("/")
    if link_mode == LINK_MODE_LOCAL:
        return LinkTargets(
            coverage_index="coverage-report/index.html",
            binary_size_report="build-binary-size/binary_size_report.html",
            binary_size_bargraph="build-binary-size/binary_size_bargraph.svg",
        )
    if link_mode == LINK_MODE_SITE:
        return LinkTargets(
            coverage_index=f"{base}/reports/coverage/index.html",
            binary_size_report=f"{base}/reports/binary-size/binary_size_report.html",
            binary_size_bargraph=f"{base}/reports/binary-size/binary_size_bargraph.svg",
        )
    if link_mode == LINK_MODE_DOCS:
        # Coverage is shipped as a zip and only exists as a browsable tree
        # on the deployed docs site, so always link to the site URL.
        return LinkTargets(
            coverage_index=f"{base}/reports/coverage/index.html",
            binary_size_report="reports/binary-size/binary_size_report.html",
            binary_size_bargraph="reports/binary-size/binary_size_bargraph.svg",
        )
    raise ValueError(f"Unknown link_mode: {link_mode!r} (expected one of {LINK_MODES})")


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


# Where `tools/binary_size.sh` / `tools/coverage.sh` drop their HTML output
# inside the workspace. These live at the repo root and are `.gitignore`d.
_BINARY_SIZE_OUTPUT_DIR = Path("build-binary-size")
_COVERAGE_OUTPUT_DIR = Path("coverage-report")

# Filename of the coverage archive under ``docs/reports/``. The zip is
# unpacked by ``tools/build_docs.sh`` into the Doxygen HTML output tree.
COVERAGE_ARCHIVE_NAME = "coverage.zip"

# Top-level directory name inside ``coverage.zip``. ``tools/build_docs.sh``
# does ``unzip coverage.zip -d reports/``, so the zip must contain a
# ``coverage/`` directory (matching the final URL path under the docs site)
# rather than the generator's raw ``coverage-report/`` output dirname.
COVERAGE_ARCHIVE_ROOT = "coverage"

# Files from ``build-binary-size/`` that are useful to surface on the docs
# site. Everything else in that directory (stripped binaries, .debug info,
# intermediate bloaty CSVs) is many tens of MB of raw build output that
# nothing on the docs site links to, so we don't ship it.
_BINARY_SIZE_PUBLISHED_FILES: tuple[str, ...] = (
    "binary_size_report.html",
    "binary_size_bargraph.svg",
)


def _copy_binary_size_reports(reports_root: typing.Optional[Path]) -> bool:
    """Copy the publishable binary-size artifacts into ``docs/reports/``."""
    if reports_root is None:
        return False
    if not _BINARY_SIZE_OUTPUT_DIR.exists():
        return False

    destination = reports_root / "binary-size"
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True, exist_ok=True)

    copied_any = False
    for name in _BINARY_SIZE_PUBLISHED_FILES:
        source = _BINARY_SIZE_OUTPUT_DIR / name
        if source.exists():
            shutil.copyfile(source, destination / name)
            copied_any = True
    return copied_any


def _archive_coverage_reports(reports_root: typing.Optional[Path]) -> bool:
    """Archive the lcov HTML tree as a single zip under ``docs/reports/``.

    The raw tree is ~26 MB spread across hundreds of files, which bloats
    the checked-in repo and confuses fuzzy file-name search. A single
    ``coverage.zip`` compresses to a few MB and is transparent to Doxygen
    (which excludes ``docs/reports``); ``tools/build_docs.sh`` unpacks it
    into the generated HTML tree at docs-deploy time.

    Entries inside the archive are rooted at ``COVERAGE_ARCHIVE_ROOT`` so
    that ``unzip coverage.zip -d <dest>`` produces ``<dest>/coverage/…``.
    """
    if reports_root is None:
        return False
    if not _COVERAGE_OUTPUT_DIR.exists():
        return False

    reports_root.mkdir(parents=True, exist_ok=True)
    archive_path = reports_root / COVERAGE_ARCHIVE_NAME
    if archive_path.exists():
        archive_path.unlink()

    with zipfile.ZipFile(
        archive_path, mode="w", compression=zipfile.ZIP_DEFLATED
    ) as zf:
        for source in sorted(_COVERAGE_OUTPUT_DIR.rglob("*")):
            if not source.is_file():
                continue
            rel = source.relative_to(_COVERAGE_OUTPUT_DIR)
            arcname = f"{COVERAGE_ARCHIVE_ROOT}/{rel.as_posix()}"
            zf.write(source, arcname=arcname)
    return archive_path.exists()


def make_binary_size_section(
    runner: CommandRunner,
    reports_root: typing.Optional[Path],
    links: LinkTargets,
) -> SectionResult:
    args = ["tools/binary_size.sh"]
    result = runner.run("binary-size", args)
    lines = [f"Generated with: `{format_command(args)}`", ""]

    if result.success:
        lines.append(f"Full report: [binary_size_report.html]({links.binary_size_report})")
        lines.append("")

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
        copied = _copy_binary_size_reports(reports_root)
        if copied or reports_root is None:
            # In `local` mode the bargraph lives under build-binary-size/; in
            # `docs`/`site` modes it lives under the docs-site reports tree.
            content += f"\n\n![Binary size bar graph]({links.binary_size_bargraph})"

    return SectionResult("Binary Size", _result_status(result), result.duration_sec, content)


def make_code_coverage_section(
    runner: CommandRunner,
    reports_root: typing.Optional[Path],
    links: LinkTargets,
) -> SectionResult:
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

    content = _render_command_block(
        format_command(args),
        result,
        empty_output_message="(coverage.sh produced no output)",
        failure_hint=failure_hint,
    )

    if result.success:
        _archive_coverage_reports(reports_root)
        content = (
            f"Full report: [coverage-report/index.html]({links.coverage_index})\n\n"
            + content
        )

    return SectionResult(
        "Code Coverage",
        _result_status(result),
        result.duration_sec,
        content,
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
    DependencyVariant(
        category_name="editor (skia + text-full + imgui/glfw/tracy)",
        configs=("--config=skia", "--config=text-full"),
        notice_target="//third_party/licenses:notice_editor",
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
    reports_root: typing.Optional[Path] = None,
    *,
    link_mode: str = LINK_MODE_LOCAL,
    runner: typing.Optional[CommandRunner] = None,
    metadata: typing.Optional[ReportMetadata] = None,
    command_line: typing.Optional[str] = None,
) -> str:
    command_line = command_line or " ".join(sys.argv)
    metadata = metadata or gather_report_metadata(command_line)
    runner = runner or CommandRunner()
    links = _resolve_link_targets(link_mode)

    sections = [make_lines_of_code_section(runner)]
    if options.all or options.binary_size:
        sections.append(make_binary_size_section(runner, reports_root, links))
    if options.all or options.coverage:
        sections.append(make_code_coverage_section(runner, reports_root, links))
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
    parser.add_argument(
        "--link-mode",
        choices=LINK_MODES,
        default=None,
        help=(
            "How to reference coverage / binary-size HTML artifacts. "
            "'docs' uses relative reports/... paths (the default when "
            "--save docs/build_report.md); 'local' points at the raw "
            "coverage-report/ and build-binary-size/ output dirs; "
            "'site' uses absolute URLs to the published docs site."
        ),
    )
    parser.add_argument(
        "--reports-root",
        type=str,
        default=None,
        help=(
            "Directory to receive checked-in copies of the coverage and "
            "binary-size HTML trees (defaults to docs/reports/ when saving "
            "to docs/build_report.md, else no copy)."
        ),
    )
    return parser.parse_args(argv)


def _default_link_mode(save_path: typing.Optional[Path]) -> str:
    if save_path is None:
        return LINK_MODE_LOCAL
    try:
        resolved = save_path.resolve()
        docs_build_report = Path("docs/build_report.md").resolve()
    except OSError:
        return LINK_MODE_LOCAL
    if resolved == docs_build_report:
        return LINK_MODE_DOCS
    return LINK_MODE_LOCAL


def _default_reports_root(
    save_path: typing.Optional[Path], link_mode: str
) -> typing.Optional[Path]:
    # Only populate docs/reports/ when we're actually writing the checked-in
    # build_report.md in docs-link mode — otherwise stay out of the way.
    if link_mode != LINK_MODE_DOCS:
        return None
    if save_path is None:
        return None
    try:
        resolved = save_path.resolve()
        docs_build_report = Path("docs/build_report.md").resolve()
    except OSError:
        return None
    if resolved != docs_build_report:
        return None
    return Path("docs/reports")


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
    link_mode = args.link_mode or _default_link_mode(save_path)
    if args.reports_root is not None:
        reports_root: typing.Optional[Path] = Path(args.reports_root)
    else:
        reports_root = _default_reports_root(save_path, link_mode)

    report = create_build_report(
        options,
        reports_root=reports_root,
        link_mode=link_mode,
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
