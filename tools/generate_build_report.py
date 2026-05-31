from __future__ import annotations

import argparse
import json
import platform
import re
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
    doxygen_index: str


def _resolve_link_targets(link_mode: str) -> LinkTargets:
    base = DOCS_SITE_BASE_URL.rstrip("/")
    if link_mode == LINK_MODE_LOCAL:
        return LinkTargets(
            coverage_index="coverage-report/index.html",
            binary_size_report="build-binary-size/binary_size_report.html",
            binary_size_bargraph="build-binary-size/binary_size_bargraph.svg",
            # The local Doxygen output (built by `tools/build_docs.sh`) lands
            # in generated-doxygen/html/ at the repo root.
            doxygen_index="generated-doxygen/html/index.html",
        )
    if link_mode == LINK_MODE_SITE:
        return LinkTargets(
            coverage_index=f"{base}/reports/coverage/index.html",
            binary_size_report=f"{base}/reports/binary-size/binary_size_report.html",
            binary_size_bargraph=f"{base}/reports/binary-size/binary_size_bargraph.svg",
            doxygen_index=f"{base}/index.html",
        )
    if link_mode == LINK_MODE_DOCS:
        # Coverage is shipped as a zip and only exists as a browsable tree
        # on the deployed docs site, so always link to the site URL.
        return LinkTargets(
            coverage_index=f"{base}/reports/coverage/index.html",
            binary_size_report="reports/binary-size/binary_size_report.html",
            binary_size_bargraph="reports/binary-size/binary_size_bargraph.svg",
            # The checked-in build_report.md is itself rendered by Doxygen, so
            # the API docs root is one level up from this page on the site.
            doxygen_index=f"{base}/index.html",
        )
    raise ValueError(f"Unknown link_mode: {link_mode!r} (expected one of {LINK_MODES})")


@dataclass(frozen=True)
class ReportOptions:
    all: bool = False
    binary_size: bool = False
    coverage: bool = False
    tests: bool = False
    documentation: bool = False
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
}


# Override the `license_kinds` reported for packages whose upstream overlay
# uses the generic `@rules_license//licenses/generic:notice` placeholder
# instead of a specific SPDX identifier. The NOTICE.txt still carries the
# verbatim license text from upstream; this only affects the display in the
# build report.
_PACKAGE_LICENSE_KIND_OVERRIDES: dict[str, tuple[str, ...]] = {}


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
    runner: CommandRunner, query_target: str, configs: typing.Sequence[str]
) -> tuple[typing.List[str], CommandResult]:
    args = [
        "bazel",
        "cquery",
        f"deps({query_target})",
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


def make_feature_loc_section(runner: CommandRunner) -> SectionResult:
    args = ["tools/feature_loc.py"]
    result = runner.run("feature-loc", args)

    if result.success:
        if result.stdout:
            content = f"Generated with: `{format_command(args)}`\n\n{result.stdout}"
        else:
            content = (
                f"Generated with: `{format_command(args)}`\n\n"
                "(feature_loc.py produced no output)"
            )
    else:
        content = _render_command_block(
            format_command(args),
            result,
            empty_output_message="(feature_loc.py produced no output)",
        )

    return SectionResult(
        "Feature LOC",
        _result_status(result),
        result.duration_sec,
        content,
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


def _copy_bargraph_next_to_save(save_dir: Path) -> typing.Optional[str]:
    """Copy ``binary_size_bargraph.svg`` into ``save_dir``.

    Mirrors the pre-refactor behaviour: when the build report is written
    outside the workspace (``--save /tmp/report.md``) the bargraph must be
    alongside the markdown for the relative image link to resolve.
    Returns the basename to use as the image-link target, or None if no
    copy was made.
    """
    source = _BINARY_SIZE_OUTPUT_DIR / "binary_size_bargraph.svg"
    if not source.exists():
        return None
    save_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, save_dir / "binary_size_bargraph.svg")
    return "binary_size_bargraph.svg"


def make_binary_size_section(
    runner: CommandRunner,
    reports_root: typing.Optional[Path],
    links: LinkTargets,
    *,
    local_asset_dir: typing.Optional[Path] = None,
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
        # If the report is being saved outside the workspace, copy the
        # bargraph next to the saved markdown and override the image link
        # to the bare filename, so the image renders from any viewer.
        bargraph_link = links.binary_size_bargraph
        if local_asset_dir is not None:
            override = _copy_bargraph_next_to_save(local_asset_dir)
            if override is not None:
                bargraph_link = override
        if copied or reports_root is None:
            content += f"\n\n![Binary size bar graph]({bargraph_link})"

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


# Directory under ``docs/reports/`` that receives copies of the
# SVG-vs-golden image artifacts from failed image-comparison tests, so they
# render on the docs site and from GitHub's web view of the report.
_TEST_FAILURE_IMAGE_DIR = "test-failures"

# Image-comparison fixtures drop these per-case PNGs into a failing test's
# ``$TEST_UNDECLARED_OUTPUTS_DIR`` (see
# donner/svg/renderer/tests/ImageComparisonTestFixture.cc): ``expected_*`` is
# the golden, ``actual_*`` is what the renderer produced, ``diff_*`` is the
# pixelmatch diff. The ``parity_*`` trio is the Geode-vs-tiny variant.
_FAILURE_IMAGE_PREFIXES = (
    "diff_",
    "actual_",
    "expected_",
    "parity_diff_",
    "parity_geode_",
    "parity_tiny_",
)


def _target_to_testlogs_subpath(target: str) -> str:
    """Map a Bazel label to its ``bazel-testlogs`` relative directory.

    ``//donner/svg/renderer/tests:renderer_geode_golden_tests`` →
    ``donner/svg/renderer/tests/renderer_geode_golden_tests``.
    """
    package, _, name = target.lstrip("/").partition(":")
    return f"{package}/{name}" if name else package


def _collect_failure_images_for_target(test_outputs_dir: Path) -> typing.List[Path]:
    """Return the image-comparison PNGs a failed target left behind.

    A target's undeclared outputs live under ``<testlogs>/<path>/test.outputs``
    either as loose files or (when there is more than one) inside
    ``outputs.zip``. We surface the loose-file case directly; callers extract
    the zip case first.
    """
    images: typing.List[Path] = []
    if not test_outputs_dir.is_dir():
        return images
    for path in sorted(test_outputs_dir.rglob("*.png")):
        if path.name.startswith(_FAILURE_IMAGE_PREFIXES):
            images.append(path)
    return images


def _harvest_failed_image_artifacts(
    failed_targets: typing.Sequence[str],
    destination: Path,
    bazel_testlogs: Path,
) -> dict[str, typing.List[str]]:
    """Copy SVG-vs-golden PNGs from failed targets into ``destination``.

    Returns ``{target: [relative_png_path, ...]}`` for every failed target
    that produced image-comparison artifacts. Paths are relative to
    ``destination`` so the report can build links from its ``reports/`` root.
    Targets that produced no such PNGs (non-image failures) are omitted.
    """
    harvested: dict[str, typing.List[str]] = {}
    for target in failed_targets:
        subpath = _target_to_testlogs_subpath(target)
        outputs_dir = bazel_testlogs / subpath / "test.outputs"
        if not outputs_dir.exists():
            continue

        # Materialize a flat working copy: loose PNGs plus anything inside a
        # bundled outputs.zip.
        staged: typing.List[tuple[str, Path]] = []
        for path in _collect_failure_images_for_target(outputs_dir):
            staged.append((path.name, path))

        zip_path = outputs_dir / "outputs.zip"
        extracted_dir: typing.Optional[Path] = None
        if zip_path.is_file():
            extracted_dir = destination / subpath / "_unzip"
            try:
                with zipfile.ZipFile(zip_path) as zf:
                    for member in zf.namelist():
                        base = Path(member).name
                        if base.startswith(_FAILURE_IMAGE_PREFIXES) and base.endswith(
                            ".png"
                        ):
                            zf.extract(member, extracted_dir)
                            staged.append((base, extracted_dir / member))
            except (OSError, zipfile.BadZipFile):
                pass

        if not staged:
            if extracted_dir is not None and extracted_dir.exists():
                shutil.rmtree(extracted_dir, ignore_errors=True)
            continue

        target_dir = destination / subpath
        target_dir.mkdir(parents=True, exist_ok=True)
        rel_paths: typing.List[str] = []
        for name, source in staged:
            dest_file = target_dir / name
            shutil.copyfile(source, dest_file)
            rel_paths.append(dest_file.relative_to(destination).as_posix())

        if extracted_dir is not None and extracted_dir.exists():
            shutil.rmtree(extracted_dir, ignore_errors=True)

        if rel_paths:
            harvested[target] = sorted(set(rel_paths))
    return harvested


def _resolve_bazel_testlogs() -> typing.Optional[Path]:
    try:
        out = subprocess.check_output(["bazel", "info", "bazel-testlogs"], text=True).strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    path = Path(out)
    return path if path.exists() else None


def _render_failed_image_comparisons(
    failed_targets: typing.Sequence[str],
    reports_root: typing.Optional[Path],
    *,
    bazel_testlogs: typing.Optional[Path] = None,
) -> str:
    """Render the SVG-vs-golden block for failed image-comparison tests.

    When ``reports_root`` is set (docs-link mode) the PNGs are copied into
    ``reports/test-failures/`` and the report embeds them. Returns an empty
    string when there are no failed targets, no artifacts, or no place to
    stage them.
    """
    if not failed_targets:
        return ""
    if reports_root is None:
        # Without a checked-in reports tree we have nowhere to publish the
        # PNGs, so just name the failing targets.
        listed = "\n".join(f"- `{t}`" for t in failed_targets)
        return (
            "### Failed image comparisons\n\n"
            "The following targets failed; regenerate with `--save "
            "docs/build_report.md` to embed their SVG-vs-golden diffs.\n\n"
            f"{listed}"
        )

    if bazel_testlogs is None:
        bazel_testlogs = _resolve_bazel_testlogs()
    if bazel_testlogs is None:
        return ""

    destination = reports_root / _TEST_FAILURE_IMAGE_DIR
    if destination.exists():
        shutil.rmtree(destination)

    harvested = _harvest_failed_image_artifacts(failed_targets, destination, bazel_testlogs)
    if not harvested:
        # No image-comparison artifacts (the failures were non-visual). Leave
        # the staging dir clean and emit nothing.
        if destination.exists() and not any(destination.iterdir()):
            destination.rmdir()
        return ""

    lines = [
        "### Failed image comparisons",
        "",
        "SVG-vs-golden artifacts for image-comparison tests that failed. "
        "`expected_*` is the golden, `actual_*` is the rendered output, and "
        "`diff_*` is the pixelmatch difference.",
        "",
    ]
    for target in sorted(harvested):
        lines.append(f"#### `{target}`")
        lines.append("")
        for rel in harvested[target]:
            # Build the link relative to docs/ (the report lives at
            # docs/build_report.md, reports_root is docs/reports).
            href = f"reports/{_TEST_FAILURE_IMAGE_DIR}/{Path(rel).as_posix()}"
            caption = Path(rel).name
            lines.append(f"![{caption}]({href})")
        lines.append("")
    return "\n".join(lines).rstrip()


def make_documentation_section(links: LinkTargets) -> SectionResult:
    """A static section linking the Doxygen-generated API documentation.

    The API docs are produced by `tools/build_docs.sh` (which runs
    `doxygen Doxyfile`) and published to the docs site; there is no command to
    run here, so this section is informational and always reports success.
    """
    content = "\n".join(
        [
            "Doxygen-generated API documentation for the Donner SVG library.",
            "",
            f"Browse: [API documentation]({links.doxygen_index})",
            "",
            "Regenerate locally with `tools/build_docs.sh` "
            "(output: `generated-doxygen/html/`).",
        ]
    )
    return SectionResult("Documentation", "success", 0.0, content)


@dataclass(frozen=True)
class TestCaseResult:
    """One target's pass/fail/skip status as parsed from `bazel test` output."""

    target: str
    status: str  # PASSED / FAILED / FLAKY / TIMEOUT / SKIPPED / NO STATUS / ...
    detail: str  # e.g. "in 1.2s", "(cached)", or "" when absent.


# `bazel test` summary lines look like:
#   //pkg:target                                       PASSED in 1.2s
#   //pkg:target                                       FAILED in 0.3s
#   //pkg:target                                       SKIPPED
#   //pkg:target                              (cached) PASSED in 0.0s
# Statuses bazel emits in the per-target summary block.
_BAZEL_TEST_STATUSES = (
    "PASSED",
    "FAILED",
    "FLAKY",
    "TIMEOUT",
    "SKIPPED",
    "NO STATUS",
    "FAILED TO BUILD",
)
_BAZEL_TEST_LINE_RE = re.compile(
    r"^(?P<target>//\S+)\s+"
    # Bazel inserts a right-aligned "(cached)" marker between the target and
    # the status for cached results; capture it as part of the detail.
    r"(?P<cached>\(cached\)\s+)?"
    r"(?P<status>" + "|".join(re.escape(s) for s in _BAZEL_TEST_STATUSES) + r")"
    r"(?P<detail>.*)$"
)


def parse_bazel_test_results(stdout: str) -> typing.List[TestCaseResult]:
    """Extract per-target pass/fail results from `bazel test` summary output.

    Bazel prints one line per test target in its end-of-run summary; parse
    those into structured records. Lines that don't match the target/status
    shape (build progress, the trailing "Executed N out of M tests" line) are
    ignored. The result is de-duplicated by target (last status wins) so a
    target that appears in both a per-shard line and the summary isn't double
    counted.
    """
    by_target: dict[str, TestCaseResult] = {}
    for line in stdout.splitlines():
        match = _BAZEL_TEST_LINE_RE.match(line.strip())
        if match is None:
            continue
        detail = match.group("detail").strip()
        if match.group("cached"):
            detail = f"(cached) {detail}".strip()
        by_target[match.group("target")] = TestCaseResult(
            target=match.group("target"),
            status=match.group("status"),
            detail=detail,
        )
    return sorted(by_target.values(), key=lambda r: (r.status != "FAILED", r.target))


# Order test-status groups so failures/flakes surface at the top of the table.
_TEST_STATUS_ORDER = (
    "FAILED",
    "FAILED TO BUILD",
    "TIMEOUT",
    "FLAKY",
    "NO STATUS",
    "PASSED",
    "SKIPPED",
)


def _render_test_results_table(results: typing.Sequence[TestCaseResult]) -> str:
    if not results:
        return "(no per-target test results parsed from output)"

    counts: dict[str, int] = {}
    for record in results:
        counts[record.status] = counts.get(record.status, 0) + 1

    order = {status: index for index, status in enumerate(_TEST_STATUS_ORDER)}

    def status_key(status: str) -> tuple[int, str]:
        return (order.get(status, len(order)), status)

    summary = ", ".join(
        f"{count} {status.lower()}"
        for status, count in sorted(counts.items(), key=lambda kv: status_key(kv[0]))
    )

    lines = [
        f"{len(results)} targets: {summary}.",
        "",
        "| Target | Result |",
        "| --- | --- |",
    ]
    for record in sorted(results, key=lambda r: (status_key(r.status), r.target)):
        detail = f" {record.detail}" if record.detail else ""
        lines.append(f"| `{record.target}` | {record.status}{detail} |")
    return "\n".join(lines)


def make_tests_section(
    runner: CommandRunner,
    reports_root: typing.Optional[Path] = None,
    *,
    bazel_testlogs: typing.Optional[Path] = None,
) -> SectionResult:
    args = ["bazel", "test", "//donner/..."]
    result = runner.run("tests", args)

    parsed = parse_bazel_test_results(result.stdout)
    failed_targets = [r.target for r in parsed if r.status in ("FAILED", "TIMEOUT")]

    lines = [
        f"Generated with: `{format_command(args)}`",
        "",
        "### Test results",
        "",
        _render_test_results_table(parsed),
    ]

    failure_section = _render_failed_image_comparisons(
        failed_targets, reports_root, bazel_testlogs=bazel_testlogs
    )
    if failure_section:
        lines.extend(["", failure_section])

    # Preserve the raw bazel output for anyone who needs the full log.
    lines.extend(
        [
            "",
            "<details><summary>Raw <code>bazel test</code> output</summary>",
            "",
            _render_command_block(
                format_command(args),
                result,
                empty_output_message="(test command produced no output)",
            ),
            "",
            "</details>",
        ]
    )

    return SectionResult(
        "Tests",
        _result_status(result),
        result.duration_sec,
        "\n".join(lines),
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
    query_target: str
    configs: tuple[str, ...]
    notice_target: str


_DEPENDENCY_VARIANTS: tuple[DependencyVariant, ...] = (
    DependencyVariant(
        category_name="Default (tiny-skia)",
        query_target="//examples:svg_to_png",
        configs=(),
        notice_target="//third_party/licenses:notice_default",
    ),
    DependencyVariant(
        category_name="tiny-skia + text-full",
        query_target="//examples:svg_to_png",
        configs=("--config=text-full",),
        notice_target="//third_party/licenses:notice_text_full",
    ),
    DependencyVariant(
        category_name="editor (tiny-skia + imgui/glfw/tracy + editor fonts)",
        query_target="//donner/editor:editor",
        configs=(),
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
            f"deps({variant.query_target})",
        ] + list(variant.configs)
        command_display = format_command(command)
        dependencies, result = query_external_dependencies(
            runner, variant.query_target, variant.configs
        )
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
    local_asset_dir: typing.Optional[Path] = None,
    runner: typing.Optional[CommandRunner] = None,
    metadata: typing.Optional[ReportMetadata] = None,
    command_line: typing.Optional[str] = None,
) -> str:
    command_line = command_line or " ".join(sys.argv)
    metadata = metadata or gather_report_metadata(command_line)
    runner = runner or CommandRunner()
    links = _resolve_link_targets(link_mode)

    sections = [make_lines_of_code_section(runner), make_feature_loc_section(runner)]
    if options.all or options.binary_size:
        sections.append(
            make_binary_size_section(
                runner, reports_root, links, local_asset_dir=local_asset_dir
            )
        )
    if options.all or options.coverage:
        sections.append(make_code_coverage_section(runner, reports_root, links))
    if options.all or options.tests:
        sections.append(make_tests_section(runner, reports_root))
    if options.all or options.documentation:
        sections.append(make_documentation_section(links))
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
        "--documentation",
        action="store_true",
        help="Generate the documentation (Doxygen link) section",
    )
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
        documentation=args.documentation,
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

    # In local mode with an explicit --save path, stage the bargraph next
    # to the saved markdown so the image renders even when the report
    # lives outside the workspace (e.g. --save /tmp/report.md).
    local_asset_dir: typing.Optional[Path] = None
    if link_mode == LINK_MODE_LOCAL and save_path is not None:
        local_asset_dir = save_path.parent

    report = create_build_report(
        options,
        reports_root=reports_root,
        link_mode=link_mode,
        local_asset_dir=local_asset_dir,
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
