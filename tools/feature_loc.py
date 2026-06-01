#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import fnmatch
import subprocess
import sys
import typing
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class FeatureDefinition:
    label: str
    description: str
    patterns: tuple[str, ...]


@dataclass(frozen=True)
class FileLoc:
    path: str
    code: int


@dataclass
class FeatureStats:
    files: int = 0
    product_loc: int = 0
    support_loc: int = 0

    @property
    def total_loc(self) -> int:
        return self.product_loc + self.support_loc


FEATURES: tuple[FeatureDefinition, ...] = (
    FeatureDefinition(
        label="Editor",
        description="ImGui/GLFW editor, async rendering, repro, and sandbox tooling",
        patterns=("donner/editor/**",),
    ),
    FeatureDefinition(
        label="Composited rendering",
        description="Layer promotion, dual-path verification, and compositor hints",
        patterns=("donner/svg/compositor/**",),
    ),
    FeatureDefinition(
        label="Text",
        description="SVG text DOM, layout, shaping, font loading, and font parsers",
        patterns=(
            "donner/base/fonts/**",
            "donner/css/FontFace.h",
            "donner/svg/components/resources/FontResource.h",
            "donner/svg/components/text/**",
            "donner/svg/core/Font*.h",
            "donner/svg/core/Text*.h",
            "donner/svg/core/tests/Text*.cc",
            "donner/svg/resources/Font*.cc",
            "donner/svg/resources/Font*.h",
            "donner/svg/resources/tests/Font*.cc",
            "donner/svg/SVGText*.cc",
            "donner/svg/SVGText*.h",
            "donner/svg/SVGTSpanElement.cc",
            "donner/svg/SVGTSpanElement.h",
            "donner/svg/tests/SVGText*.cc",
            "donner/svg/tests/SVGTSpanElement_tests.cc",
            "donner/svg/text/**",
            "donner/svg/renderer/tests/MockTextBackend.h",
            "donner/svg/renderer/tests/Text*.cc",
        ),
    ),
    FeatureDefinition(
        label="Filters",
        description="SVG filter DOM, filter graph components, and CPU/GPU filter engines",
        patterns=(
            "donner/svg/components/filter/**",
            "donner/svg/renderer/FilterGraphExecutor.cc",
            "donner/svg/renderer/FilterGraphExecutor.h",
            "donner/svg/renderer/geode/GeodeFilterEngine.cc",
            "donner/svg/renderer/geode/GeodeFilterEngine.h",
            "donner/svg/renderer/tests/FilterGraphExecutor_tests.cc",
            "donner/svg/renderer/tests/ZoomFilterRepro_tests.cc",
            "donner/svg/SVGFE*.cc",
            "donner/svg/SVGFE*.h",
            "donner/svg/SVGFilter*.cc",
            "donner/svg/SVGFilter*.h",
            "donner/svg/tests/SVGFilter*.cc",
        ),
    ),
    FeatureDefinition(
        label="Rendering",
        description="Renderer facade, renderer driver, backends, and image comparison helpers",
        patterns=("donner/svg/renderer/**",),
    ),
    FeatureDefinition(
        label="SVG document model",
        description="SVG DOM wrappers, parser, properties, ECS components, resources, and tools",
        patterns=(
            "donner/svg/**",
        ),
    ),
    FeatureDefinition(
        label="CSS",
        description="CSS values, parsing, selector matching, and declarations",
        patterns=("donner/css/**",),
    ),
    FeatureDefinition(
        label="Base/shared",
        description="Core utilities, geometry, XML, parsers, encoding, and element helpers",
        patterns=("donner/base/**",),
    ),
    FeatureDefinition(
        label="Benchmarks",
        description="Benchmark harnesses under donner/benchmarks",
        patterns=("donner/benchmarks/**",),
    ),
    FeatureDefinition(
        label="Other",
        description="Files not covered by a more specific feature bucket",
        patterns=("**",),
    ),
)


EDITOR_BREAKDOWN: tuple[FeatureDefinition, ...] = (
    FeatureDefinition(
        label="Editor app and shell",
        description=(
            "Main entrypoints, top-level app wiring, menus, sidebars, dialogs, "
            "and command queue"
        ),
        patterns=(
            "donner/editor/app/**",
            "donner/editor/main.cc",
            "donner/editor/CommandQueue.*",
            "donner/editor/DialogPresenter.*",
            "donner/editor/EditorApp.*",
            "donner/editor/EditorCommand.h",
            "donner/editor/EditorShell.*",
            "donner/editor/KeyboardShortcutPolicy.h",
            "donner/editor/MenuBarPresenter.*",
            "donner/editor/SidebarPresenter.*",
            "donner/editor/tests/CommandQueue_tests.cc",
            "donner/editor/tests/EditorApp_tests.cc",
            "donner/editor/tests/KeyboardShortcutPolicy_tests.cc",
        ),
    ),
    FeatureDefinition(
        label="Editor host UI",
        description="GLFW/ImGui host integration, clipboard, profiling wrapper, and window glue",
        patterns=(
            "donner/editor/gui/**",
            "donner/editor/ClipboardInterface.h",
            "donner/editor/ImGui*.cc",
            "donner/editor/ImGui*.h",
            "donner/editor/InMemoryClipboard.h",
            "donner/editor/TracyWrapper.h",
            "donner/editor/tests/ClipboardInterface_tests.cc",
            "donner/editor/tests/EditorWindow_tests.cc",
        ),
    ),
    FeatureDefinition(
        label="Document sync and undo",
        description=(
            "SVG document state, source sync, attribute writeback, "
            "incremental reparse, and undo"
        ),
        patterns=(
            "donner/editor/AsyncSVGDocument.*",
            "donner/editor/AttributeWriteback.*",
            "donner/editor/DocumentSyncController.*",
            "donner/editor/SourceSync.*",
            "donner/editor/UndoTimeline.*",
            "donner/editor/tests/AsyncSVGDocument_tests.cc",
            "donner/editor/tests/AttributeWriteback_tests.cc",
            "donner/editor/tests/DocumentSyncController_tests.cc",
            "donner/editor/tests/EditorSync_tests.cc",
            "donner/editor/tests/UndoTimeline_tests.cc",
        ),
    ),
    FeatureDefinition(
        label="Viewport and tools",
        description="Viewport math, pointer gestures, selection, overlays, and interactive tools",
        patterns=(
            "donner/editor/EditorInputBridge.*",
            "donner/editor/ExperimentalDragPresentation.h",
            "donner/editor/OverlayRenderer.*",
            "donner/editor/PinchEventMonitor*",
            "donner/editor/RenderPane*",
            "donner/editor/SelectionAabb.*",
            "donner/editor/SelectTool.*",
            "donner/editor/Tool.h",
            "donner/editor/Viewport*",
            "donner/editor/tests/DragReleasePopBack_tests.cc",
            "donner/editor/tests/ExperimentalDragPresentation_tests.cc",
            "donner/editor/tests/OverlayRenderer_tests.cc",
            "donner/editor/tests/RenderPane*.cc",
            "donner/editor/tests/SelectionAabb_tests.cc",
            "donner/editor/tests/SelectTool_tests.cc",
            "donner/editor/tests/Viewport*.cc",
        ),
    ),
    FeatureDefinition(
        label="Async rendering",
        description=(
            "Editor render coordination, async worker path, texture cache, and "
            "render perf repros"
        ),
        patterns=(
            "donner/editor/AsyncRenderer.*",
            "donner/editor/GlTextureCache.*",
            "donner/editor/RenderCoordinator.*",
            "donner/editor/tests/AsyncRenderer*",
            "donner/editor/tests/FilterDragRepro*",
        ),
    ),
    FeatureDefinition(
        label="Text editor",
        description="Standalone source text editor, text buffer, text patching, and related tests",
        patterns=(
            "donner/editor/Text*",
            "donner/editor/tests/Text*.cc",
        ),
    ),
    FeatureDefinition(
        label="Repro files",
        description="User-visible repro recording and .donner-repro encoding/decoding",
        patterns=("donner/editor/repro/**",),
    ),
    FeatureDefinition(
        label="Sandbox and replay",
        description=(
            "Parser/render sandbox processes, wire format, record/replay, and "
            "structural diffs"
        ),
        patterns=("donner/editor/sandbox/**",),
    ),
    FeatureDefinition(
        label="Editor misc and integration tests",
        description="Editor files not covered by a more specific editor sub-bucket",
        patterns=("donner/editor/**",),
    ),
)


RENDERING_BREAKDOWN: tuple[FeatureDefinition, ...] = (
    FeatureDefinition(
        label="Renderer API and contracts",
        description=(
            "Public renderer facade, backend selection glue, interface contracts, "
            "gradients, and stroke params"
        ),
        patterns=(
            "donner/svg/renderer/Renderer.cc",
            "donner/svg/renderer/Renderer.h",
            "donner/svg/renderer/RendererGeodeBackend.cc",
            "donner/svg/renderer/RendererInterface.h",
            "donner/svg/renderer/RendererInternal.h",
            "donner/svg/renderer/RendererTinySkiaBackend.cc",
            "donner/svg/renderer/ResolvedGradient.*",
            "donner/svg/renderer/StrokeParams.h",
            "donner/svg/renderer/tests/RendererPublicApi_tests.cc",
            "donner/svg/renderer/tests/Renderer_tests.cc",
        ),
    ),
    FeatureDefinition(
        label="Traversal and render context",
        description=(
            "RendererDriver traversal, RenderingContext instantiation, utility "
            "code, and shared views"
        ),
        patterns=(
            "donner/svg/renderer/common/**",
            "donner/svg/renderer/PixelFormatUtils.*",
            "donner/svg/renderer/RendererDriver.*",
            "donner/svg/renderer/RendererUtils.*",
            "donner/svg/renderer/RenderingContext.*",
            "donner/svg/renderer/tests/MockRendererInterface.h",
            "donner/svg/renderer/tests/RendererDriver_tests.cc",
            "donner/svg/renderer/tests/RenderingContext_tests.cc",
        ),
    ),
    FeatureDefinition(
        label="TinySkia backend",
        description="TinySkia software renderer backend",
        patterns=(
            "donner/svg/renderer/RendererTinySkia.cc",
            "donner/svg/renderer/RendererTinySkia.h",
            "donner/svg/renderer/tests/RendererAscii_tests.cc",
            "donner/svg/renderer/tests/RendererTestBackendTinySkia.cc",
        ),
    ),
    FeatureDefinition(
        label="Geode backend",
        description=(
            "WebGPU/Slug renderer backend, GPU device, encoders, pipelines, "
            "shaders, and tests"
        ),
        patterns=(
            "donner/svg/renderer/geode/**",
            "donner/svg/renderer/RendererGeode.cc",
            "donner/svg/renderer/RendererGeode.h",
            "donner/svg/renderer/tests/RendererGeode*.cc",
            "donner/svg/renderer/tests/RendererTestBackendGeode.cc",
        ),
    ),
    FeatureDefinition(
        label="Image and debug utilities",
        description="Image IO, terminal image preview, and bitmap comparison helpers",
        patterns=(
            "donner/svg/renderer/RendererImageIO.*",
            "donner/svg/renderer/TerminalImageViewer.*",
            "donner/svg/renderer/tests/ImageComparison*",
            "donner/svg/renderer/tests/RendererImageTestUtils.*",
            "donner/svg/renderer/tests/TerminalImageViewer_tests.cc",
        ),
    ),
    FeatureDefinition(
        label="Wasm bridge",
        description="Renderer wasm bridge entrypoints",
        patterns=("donner/svg/renderer/wasm/**",),
    ),
    FeatureDefinition(
        label="Renderer test harness",
        description="resvg suite harness, test backend abstractions, and renderer error-path tests",
        patterns=(
            "donner/svg/renderer/benchmarks/**",
            "donner/svg/renderer/tests/**",
        ),
    ),
    FeatureDefinition(
        label="Rendering misc",
        description="Rendering files not covered by a more specific renderer sub-bucket",
        patterns=("donner/svg/renderer/**",),
    ),
)


def _matches(path: str, pattern: str) -> bool:
    return fnmatch.fnmatchcase(path, pattern)


def feature_for_path(path: str) -> FeatureDefinition:
    normalized = path.replace("\\", "/")
    for feature in FEATURES:
        if any(_matches(normalized, pattern) for pattern in feature.patterns):
            return feature
    return FEATURES[-1]


def is_support_file(path: str) -> bool:
    filename = Path(path).name
    return (
        "/tests/" in path
        or "/benchmarks/" in path
        or filename.endswith("_tests.cc")
        or filename.endswith("_test.cc")
        or filename.endswith("_fuzzer.cc")
        or filename.endswith("Bench.cc")
    )


def parse_cloc_by_file_csv(csv_text: str) -> list[FileLoc]:
    rows: list[FileLoc] = []
    for row in csv.DictReader(csv_text.splitlines()):
        filename = (row.get("filename") or "").strip()
        if not filename or filename == "SUM":
            continue

        code_text = (row.get("code") or "0").strip()
        try:
            code = int(code_text)
        except ValueError:
            continue

        rows.append(FileLoc(path=filename.replace("\\", "/"), code=code))

    return rows


def run_cloc(root: Path) -> list[FileLoc]:
    root = root.resolve()
    args = [
        "cloc",
        str(root / "donner"),
        "--include-lang=C++,C/C++ Header",
        "--timeout=60",
        "--diff-timeout=60",
        "--by-file",
        "--csv",
    ]
    output = subprocess.check_output(args, text=True)
    return normalize_paths(parse_cloc_by_file_csv(output), root)


def normalize_paths(files: typing.Iterable[FileLoc], root: Path) -> list[FileLoc]:
    normalized_files: list[FileLoc] = []
    for file_loc in files:
        path = Path(file_loc.path)
        if path.is_absolute():
            try:
                path = path.relative_to(root)
            except ValueError:
                pass
        normalized_files.append(
            FileLoc(path=path.as_posix(), code=file_loc.code)
        )

    return normalized_files


def aggregate_by_feature(files: typing.Iterable[FileLoc]) -> dict[str, FeatureStats]:
    stats = {feature.label: FeatureStats() for feature in FEATURES}

    for file_loc in files:
        feature = feature_for_path(file_loc.path)
        feature_stats = stats[feature.label]
        feature_stats.files += 1
        if is_support_file(file_loc.path):
            feature_stats.support_loc += file_loc.code
        else:
            feature_stats.product_loc += file_loc.code

    return stats


def aggregate_by_breakdown(
    files: typing.Iterable[FileLoc],
    parent_label: str,
    breakdown: typing.Sequence[FeatureDefinition],
) -> dict[str, FeatureStats]:
    stats = {feature.label: FeatureStats() for feature in breakdown}

    for file_loc in files:
        if feature_for_path(file_loc.path).label != parent_label:
            continue

        feature = _breakdown_for_path(file_loc.path, breakdown)
        feature_stats = stats[feature.label]
        feature_stats.files += 1
        if is_support_file(file_loc.path):
            feature_stats.support_loc += file_loc.code
        else:
            feature_stats.product_loc += file_loc.code

    return stats


def _breakdown_for_path(
    path: str, breakdown: typing.Sequence[FeatureDefinition]
) -> FeatureDefinition:
    normalized = path.replace("\\", "/")
    for feature in breakdown:
        if any(_matches(normalized, pattern) for pattern in feature.patterns):
            return feature
    return breakdown[-1]


def _format_int(value: int) -> str:
    return f"{value:,}"


def _render_stats_table(
    features: typing.Sequence[FeatureDefinition],
    stats: dict[str, FeatureStats],
) -> str:
    product_total = sum(feature_stats.product_loc for feature_stats in stats.values())
    support_total = sum(feature_stats.support_loc for feature_stats in stats.values())
    loc_total = product_total + support_total
    file_total = sum(feature_stats.files for feature_stats in stats.values())

    lines = [
        "| Feature | Product LOC | Support LOC | Total LOC | Files | Notes |",
        "| --- | ---: | ---: | ---: | ---: | --- |",
    ]

    for feature in features:
        feature_stats = stats[feature.label]
        if feature_stats.total_loc == 0 and (
            feature.label == "Other" or "misc" in feature.label.lower()
        ):
            continue

        lines.append(
            "| "
            + " | ".join(
                [
                    feature.label,
                    _format_int(feature_stats.product_loc),
                    _format_int(feature_stats.support_loc),
                    _format_int(feature_stats.total_loc),
                    _format_int(feature_stats.files),
                    feature.description,
                ]
            )
            + " |"
        )

    lines.append(
        "| "
        + " | ".join(
            [
                "**Total**",
                f"**{_format_int(product_total)}**",
                f"**{_format_int(support_total)}**",
                f"**{_format_int(loc_total)}**",
                f"**{_format_int(file_total)}**",
                "",
            ]
        )
        + " |"
    )

    return "\n".join(lines)


def render_markdown_table(stats: dict[str, FeatureStats]) -> str:
    lines = [
        (
            "File-level attribution by first matching feature bucket. Product LOC excludes "
            "`tests/`, fuzzer, and benchmark files; support LOC includes them. Shared files "
            "stay in the broad bucket that owns the file."
        ),
        "",
        _render_stats_table(FEATURES, stats),
    ]

    return "\n".join(lines)


def render_markdown(files: typing.Iterable[FileLoc]) -> str:
    file_list = list(files)
    lines = [
        render_markdown_table(aggregate_by_feature(file_list)),
        "",
        "### Editor Breakdown",
        "",
        _render_stats_table(
            EDITOR_BREAKDOWN,
            aggregate_by_breakdown(file_list, "Editor", EDITOR_BREAKDOWN),
        ),
        "",
        "### Rendering Breakdown",
        "",
        _render_stats_table(
            RENDERING_BREAKDOWN,
            aggregate_by_breakdown(file_list, "Rendering", RENDERING_BREAKDOWN),
        ),
    ]

    return "\n".join(lines)


def parse_args(argv: typing.Optional[typing.Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Attribute Donner C++/header LOC to broad feature buckets."
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="Workspace root containing the donner/ source tree.",
    )
    return parser.parse_args(argv)


def main(argv: typing.Optional[typing.Sequence[str]] = None) -> int:
    args = parse_args(argv)
    try:
        files = run_cloc(args.root)
    except FileNotFoundError:
        print(
            "cloc not available; install `cloc` to populate this section",
            file=sys.stderr,
        )
        return 127
    except subprocess.CalledProcessError as exc:
        print(f"cloc failed with exit code {exc.returncode}", file=sys.stderr)
        return exc.returncode or 1

    print(render_markdown(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
