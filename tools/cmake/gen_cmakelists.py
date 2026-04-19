#!/usr/bin/env python3
"""
Generate CMakeLists.txt files for Donner libraries using Bazel query.

This script performs three high-level steps:

1.  **generate_root()**
    Creates the project-level `CMakeLists.txt`, declares external
    dependencies via FetchContent (absl, EnTT, googletest, …),
    embeds Skia, and wires up umbrella and convenience libraries.

2.  **generate_all_packages()**
    Discovers every `cc_library`, `cc_binary`, `cc_test`, and `embed_resources`
    under the `//…` Bazel workspace (excluding a few hand-curated packages)
    and mirrors them as CMake targets with appropriate source files,
    include paths, and transitive dependencies.

The generated tree lets consumers build Donner without Bazel, while
retaining the original dependency graph.
"""

from __future__ import annotations

import argparse
import filecmp
import subprocess
import sys
import re
import shutil
import tempfile
from pathlib import Path
from typing import DefaultDict, Dict, List, Optional, Set, Tuple
from dataclasses import dataclass
import json
import os
import xml.etree.ElementTree as ElementTree

#
# Bazel helpers
#

# Use bzlmod-aware queries since this repository relies on MODULE.bazel.
BAZEL_PREFIX = ["bazel"]

# When running in --check mode, warn about unmapped deps instead of silently dropping them.
_check_mode = False
_unmapped_deps: List[str] = []

#
# Global state collected while emitting libraries
#

# Mapping of known Bazel labels for external/third-party deps → CMake targets.
# Adjust when an external project changes its exported target name.
KNOWN_BAZEL_TO_CMAKE_DEPS: Dict[str, str] = {
    "@com_google_absl//absl/debugging:failure_signal_handler": "absl::failure_signal_handler",
    "@com_google_absl//absl/debugging:symbolize": "absl::symbolize",
    "@com_google_gtest//:gtest_main": "gmock_main",
    "@com_google_gtest//:gtest": "gmock",
    "@entt//:entt": "EnTT::EnTT",
    "@entt//src:entt": "EnTT::EnTT",
    "@nlohmann_json//:json": "nlohmann_json::nlohmann_json",
    "@rules_cc//cc/runfiles:runfiles": "rules_cc_runfiles",
    "@stb//:image_write": "stb_image_write",
    "@stb//:image": "stb_image",
    "@stb//:truetype": "stb_truetype",
    "@pixelmatch-cpp17//:pixelmatch-cpp17": "pixelmatch-cpp17",
    "@zlib//:z": "zlib",
    "//third_party:zlib": "zlib",
    "@tiny-skia-cpp//src:tiny_skia_lib": "tiny_skia",
    "@tiny-skia-cpp//src:tiny_skia_lib_native": "tiny_skia",
    "@tiny-skia-cpp//src/tiny_skia:tiny_skia_core": "tiny_skia",
    "@tiny-skia-cpp//src/tiny_skia/filter:filter": "tiny_skia",
    "@woff2//:woff2_decode": "woff2dec",
    "@brotli//:brotlidec": "brotlidec",
    # @re2 is a test-only transitive dep via googletest; not linked separately
    # in CMake (googletest already handles it via FetchContent).
}

# Bazel external labels that appear in the dep graph but have no corresponding
# CMake target (either because they're internal implementation details of a
# FetchContent'd library, or they're test-only transitive deps). Silently drop
# these without warning.
_IGNORED_EXTERNAL_DEPS: Set[str] = {
    # re2 is pulled in as a test-only dep by googletest; FetchContent handles it.
    "@re2//:re2",
    # Skia internal module targets - the "skia" FetchContent target covers all of them.
    "@skia//src/core:core",
    "@skia//src/pathops:pathops",
    "@skia//src/ports:fontmgr_empty_freetype",
    "@skia//src/ports:fontmgr_fontconfig_freetype",
    # ImGui and GLFW are used by the SVG viewer (Bazel-only WASM target).
    "@glfw//:glfw",
    "@imgui//:imgui",
}

# Packages whose CMake build is provided manually or by FetchContent and must
# *not* be auto-generated here.
SKIPPED_PACKAGES = {
    "",  # root package - handled by generate_root()
    "third_party",  # perf-sensitive wrappers only; CMake deps resolved via KNOWN_BAZEL_TO_CMAKE_DEPS
    "donner/benchmarks",  # requires Google Benchmark (Bazel-only)
    "donner/svg/renderer/geode",  # Geode (WebGPU) — Bazel-only, gated behind --enable_dawn flag (historical name; now selects wgpu-native)
    "donner/svg/renderer/wasm",  # Emscripten WASM module (cc_binary uses --no-entry); native link fails without main()
    "third_party/emdawnwebgpu",  # Dawn's Emscripten WebGPU bindings — WASM-only; `webgpu.cpp` includes <emscripten/emscripten.h>
    "third_party/webgpu-cpp",  # wgpu-native C++ wrapper — Bazel-only, pulls webgpu.h from http_archive prebuilts
    "donner/editor",  # Donner Editor — Bazel-only, depends on imgui/glfw/tracy
    "donner/editor/app",
    "donner/editor/app/tests",
    "donner/editor/gui",
    "donner/editor/resources",
    "donner/editor/sandbox",
    "donner/editor/sandbox/tests",
    "donner/editor/tests",
    "donner/editor/wasm",
    "third_party/emscripten-glfw",
    "third_party/stb",
    "pixelmatch-cpp17",
}

# Individual targets to skip entirely from CMake generation.
SKIPPED_TARGETS: Set[str] = {
    # Freetype linking is handled by EXTRA_LINK_DEPS for text_backend_full;
    # the bare "freetype" name is not a valid CMake target.
    "//third_party:freetype",
    # svg_viewer depends on @imgui (which pulls GLFW), neither of which is
    # wired into the CMake mirror. Bazel-only, tagged manual.
    "//examples:svg_viewer",
}

# Bazel toolchain-internal deps that are not real C++ libraries and should not
# trigger "unmapped external dep" warnings. These are implicit deps that Bazel
# adds to cc_test/cc_binary targets but are handled by the CMake toolchain itself.
_BAZEL_INTERNAL_DEPS: Set[str] = {
    "@bazel_tools//tools/cpp:link_extra_lib",
    "@bazel_tools//tools/cpp:malloc",
    "@rules_cc//:link_extra_lib",
}

# External deps that are handled via EXTRA_LINK_DEPS / pkg-config in CMake
# and shouldn't warn as "unmapped". Linking happens through system find_package.
_DEPS_HANDLED_EXTERNALLY: Set[str] = {
    "@harfbuzz//:harfbuzz",
    "@freetype//:freetype",
}


# Pattern for identifying abseil-cpp deps in either the canonical
# (@@abseil-cpp+//) or legacy (@com_google_absl//) form.
_ABSL_DEP_RE = re.compile(r"^@+(?:abseil-cpp\+?|com_google_absl)//absl/")


def _is_known_bazel_internal(dep: str) -> bool:
    """Return True if *dep* is a Bazel toolchain-internal label or otherwise
    handled outside the normal target_link_libraries() path."""
    if dep in _BAZEL_INTERNAL_DEPS or dep in _DEPS_HANDLED_EXTERNALLY:
        return True
    if dep in _IGNORED_EXTERNAL_DEPS:
        return True
    # Abseil deps are silently dropped unless they appear in
    # KNOWN_BAZEL_TO_CMAKE_DEPS. Auto-mapping the rule name to absl::<rulename>
    # is unreliable: some abseil rules export under a different CMake target
    # (e.g. //absl/flags:parse → absl::flags_parse, not absl::parse), and
    # several rules don't export a CMake target at all when absl is built as
    # a subproject via FetchContent. The downstream cc_library/cc_test
    # historically pulls absl in transitively or only uses header-only parts,
    # so dropping these matches the pre-existing CMake build behavior.
    if _ABSL_DEP_RE.match(dep) and dep not in KNOWN_BAZEL_TO_CMAKE_DEPS:
        return True
    return False

# Helper constants for CMake condition strings.
_SKIA = 'DONNER_RENDERER_BACKEND STREQUAL "skia"'
_TINY_SKIA = 'DONNER_RENDERER_BACKEND STREQUAL "tiny_skia"'
_TEXT_FULL = "DONNER_TEXT_FULL"

# Maps CMake target name → CMake condition expression.
# Targets in this map are wrapped in if(<condition>) … endif().
CONDITIONAL_TARGETS: Dict[str, str] = {
    # Skia backend
    "donner_svg_renderer_renderer_skia": _SKIA,
    "donner_svg_renderer_skia_deps": _SKIA,
    "donner_svg_renderer_skia_deps_opt": _SKIA,
    "donner_svg_renderer_skia_deps_unconfigured": _SKIA,
    "donner_third_party_skia_user_config_user_config": _SKIA,
    "skia": _SKIA,
    # Skia-only tests
    "donner_svg_renderer_tests_renderer_test_utils": _SKIA,
    "donner_svg_renderer_tests_renderer_ascii_tests": _SKIA,
    "donner_svg_tests_svg_renderer_ascii_tests": _SKIA,
    # Geode backend (Bazel-only: depends on wgpu-native prebuilts fetched via
    # http_archive under //third_party/webgpu-cpp). The renderer_geode library
    # and its tests are unbuildable in CMake — the webgpu-cpp wrapper isn't
    # generated by gen_cmakelists.py, and the geode/ subpackage targets it
    # depends on are in SKIPPED_PACKAGES. Wrap in if(FALSE).
    "donner_svg_renderer_renderer_geode": "FALSE",
    "donner_svg_renderer_tests_renderer_geode_tests": "FALSE",
    "donner_svg_renderer_tests_renderer_geode_tests_impl": "FALSE",
    "donner_svg_renderer_tests_renderer_geode_golden_tests": "FALSE",
    "donner_svg_renderer_tests_renderer_geode_golden_tests_impl": "FALSE",
    # TinySkia backend
    "donner_svg_renderer_renderer_tiny_skia": _TINY_SKIA,
    # tiny-skia lib
    "tiny_skia": _TINY_SKIA,
    # Filters (tiny-skia filter graph executor)
    "donner_svg_renderer_filter_graph_executor": f"{_TINY_SKIA} AND DONNER_FILTERS",
    # Text rendering
    "donner_svg_resources_font_manager": "DONNER_TEXT",
    "donner_svg_resources_font_manager_tests": "DONNER_TEXT",
    # WOFF2
    "donner_base_fonts_woff2_parser": "DONNER_TEXT_WOFF2",
    "donner_base_fonts_woff2_parser_tests": "DONNER_TEXT_WOFF2",
    "woff2dec": "DONNER_TEXT_WOFF2",
    "stb_truetype": "DONNER_TEXT",
    # Text full tier (FreeType + HarfBuzz)
    "donner_svg_text_text_backend_full": _TEXT_FULL,
    "donner_svg_renderer_tests_text_backend_tests": _TEXT_FULL,
}

# Deps that may not exist depending on CMake options. When these appear in a
# target_link_libraries() call for an *unconditional* target they are emitted
# inside if(TARGET <dep>) guards.
OPTIONAL_DEPS: Set[str] = {
    "woff2dec",
    "stb_truetype",
    "skia",
    "tiny_skia",
    "donner_svg_renderer_renderer_skia",
    "donner_svg_renderer_renderer_tiny_skia",
    "donner_svg_renderer_renderer_geode",
    "donner_svg_renderer_filter_graph_executor",
    "donner_svg_resources_font_manager",
    "donner_base_fonts_woff2_parser",
    "donner_svg_renderer_skia_deps",
    "donner_svg_renderer_skia_deps_opt",
    "donner_svg_renderer_skia_deps_unconfigured",
    "donner_third_party_skia_user_config_user_config",
    "donner_svg_renderer_tests_renderer_test_utils",
    "donner_svg_text_text_backend_full",
    # wgpu-native C++ wrapper: Bazel-only (pulls webgpu.h from http_archive
    # prebuilts that the CMake mirror doesn't fetch). References from
    # non-conditional targets are fine because the link path is only exercised
    # when the Geode backend is selected, which is itself CMake-excluded.
    "donner_third_party_webgpu-cpp_webgpu_cpp",
}

# Bazel-only targets that live in packages intentionally skipped by the CMake
# generator. These must never be emitted as link dependencies, because the
# validator treats every target_link_libraries() reference as real even when it
# sits under an unreachable CMake guard.
SKIPPED_CMAKE_TARGET_DEPS: Set[str] = {
    "donner_svg_renderer_geode_geo_encoder",
    "donner_svg_renderer_geode_geode_device",
    "donner_svg_renderer_geode_geode_filter_engine",
    "donner_svg_renderer_geode_geode_image_pipeline",
    "donner_svg_renderer_geode_geode_pipeline",
    "donner_svg_renderer_geode_geode_path_encoder",
    "donner_svg_renderer_geode_geode_shaders",
    "donner_svg_renderer_geode_geode_texture_encoder",
    "donner_svg_renderer_geode_geode_wgpu_util",
}


def _should_skip_cmake_dep(cmake_dep: Optional[str]) -> bool:
    """Return True when a mapped CMake dep should be omitted entirely."""
    return bool(cmake_dep) and cmake_dep in SKIPPED_CMAKE_TARGET_DEPS

# Sources that must be conditionally compiled. Maps cmake target name → dict of
# source filename → CMake condition. Matched sources are removed from the main
# add_library() call and added via target_sources() inside if() blocks.
CONDITIONAL_SOURCES: Dict[str, Dict[str, str]] = {
    "donner_svg_renderer_renderer": {
        "RendererTinySkiaBackend.cc": _TINY_SKIA,
        "RendererSkiaBackend.cc": _SKIA,
        # Geode is Bazel-only — never compile its backend factory in CMake.
        "RendererGeodeBackend.cc": "FALSE",
    },
    "donner_svg_renderer_tests_renderer_test_backend": {
        "RendererTestBackendTinySkia.cc": _TINY_SKIA,
        "RendererTestBackendSkia.cc": _SKIA,
        "RendererTestBackendGeode.cc": "FALSE",
    },
}

# Compile definitions gated on CMake options.
# (cmake_target, condition, [definitions], scope)
CONDITIONAL_DEFINES: List[Tuple[str, str, List[str], str]] = [
    ("donner_svg_renderer_renderer_tiny_skia", "DONNER_FILTERS", ["DONNER_FILTERS_ENABLED"], "PUBLIC"),
    ("donner_svg_renderer_renderer_tiny_skia", "DONNER_TEXT", ["DONNER_TEXT_ENABLED"], "PUBLIC"),
    ("donner_svg_resources_font_manager", "DONNER_TEXT_WOFF2", ["DONNER_TEXT_WOFF2_ENABLED"], "PUBLIC"),
    ("donner_svg_resources_font_manager_tests", "DONNER_TEXT_WOFF2", ["DONNER_TEXT_WOFF2_ENABLED"], "PRIVATE"),
    ("donner_svg_text_text_engine", _TEXT_FULL, ["DONNER_TEXT_FULL"], "PUBLIC"),
    ("donner_svg_renderer_rendering_context", _TEXT_FULL, ["DONNER_TEXT_FULL"], "PUBLIC"),
    ("donner_svg_renderer_renderer_skia", _TEXT_FULL, ["DONNER_TEXT_FULL"], "PUBLIC"),
]

# Extra link deps to inject for specific CMake targets.
# (cmake_target, condition, link_expression)
EXTRA_LINK_DEPS: List[Tuple[str, str, str]] = [
    ("donner_svg_text_text_backend_full", _TEXT_FULL,
     "${FREETYPE_LIBRARIES} ${HARFBUZZ_LIBRARIES}"),
]

# Extra include dirs to inject for specific CMake targets.
EXTRA_INCLUDE_DIRS: List[Tuple[str, str, str]] = [
    ("donner_svg_text_text_backend_full", _TEXT_FULL,
     "${FREETYPE_INCLUDE_DIRS} ${HARFBUZZ_INCLUDE_DIRS}"),
]

#
# MODULE.bazel version extraction
#


def _find_module_bazel() -> Path:
    """Locate MODULE.bazel relative to the script or working directory."""
    # Try working directory first, then script directory
    for base in [Path.cwd(), Path(__file__).resolve().parent.parent.parent]:
        p = base / "MODULE.bazel"
        if p.exists():
            return p
    raise FileNotFoundError("Cannot find MODULE.bazel")


def _parse_git_repositories(content: str, versions: Dict[str, str]) -> None:
    """Mutate `versions` by scanning `content` for git_repository /
    new_git_repository blocks. Used for both MODULE.bazel and the
    third_party/bazel/non_bcr_deps.bzl module extension (which hides
    non-BCR deps behind dev_dependency).
    """
    for m in re.finditer(r'(?:new_)?git_repository\(([^)]+)\)', content):
        block = m.group(1)
        name_m = re.search(r'name\s*=\s*"([^"]+)"', block)
        if not name_m:
            continue
        name = name_m.group(1)
        tag_m = re.search(r'tag\s*=\s*"([^"]+)"', block)
        commit_m = re.search(r'commit\s*=\s*"([^"]+)"', block)
        if tag_m:
            versions[name] = tag_m.group(1)
        elif commit_m:
            versions[name] = commit_m.group(1)


def extract_versions_from_module_bazel() -> Dict[str, str]:
    """Parse MODULE.bazel to extract canonical dependency versions/commits.

    Returns a dict mapping dependency name to version string or git commit/tag.
    This is used to keep FetchContent declarations in sync with Bazel.

    Also parses third_party/bazel/non_bcr_deps.bzl (the dev-only module
    extension that hides Skia/HarfBuzz/WOFF2/Dawn/etc. from BCR consumers)
    so that gen_cmakelists.py can still discover the version pins for those
    deps when emitting FetchContent declarations for CMake users.
    """
    module_path = _find_module_bazel()
    content = module_path.read_text()

    versions: Dict[str, str] = {}

    # Match bazel_dep(name = "...", version = "...")
    for m in re.finditer(
        r'bazel_dep\(\s*name\s*=\s*"([^"]+)"\s*,\s*(?:repo_name\s*=\s*"[^"]+"\s*,\s*)?version\s*=\s*"([^"]+)"',
        content,
    ):
        versions[m.group(1)] = m.group(2)

    # git_repository / new_git_repository blocks in MODULE.bazel proper.
    _parse_git_repositories(content, versions)

    # Also scan the non_bcr_deps module extension, which is where
    # skia/harfbuzz/woff2/dawn/resvg-test-suite/bazel_clang_tidy live now
    # that they are hidden from BCR consumers.
    non_bcr_path = module_path.parent / "third_party" / "bazel" / "non_bcr_deps.bzl"
    if non_bcr_path.exists():
        _parse_git_repositories(non_bcr_path.read_text(), versions)

    return versions


# Map from MODULE.bazel dep name to (FetchContent name, git URL, use_v_prefix).
# use_v_prefix: True for repos whose git tags have a 'v' prefix (e.g., v1.17.0),
#               False for repos that use bare versions (e.g., 0.2.17).
_MODULE_TO_FETCHCONTENT: Dict[str, Tuple[str, str, bool]] = {
    "googletest": ("googletest", "https://github.com/google/googletest.git", True),
    "nlohmann_json": ("nlohmann_json", "https://github.com/nlohmann/json.git", True),
    "zlib": ("zlib", "https://github.com/madler/zlib.git", True),
    "rules_cc": ("rules_cc", "https://github.com/bazelbuild/rules_cc.git", False),
    "pixelmatch-cpp17": (
        "pixelmatch-cpp17",
        "https://github.com/jwmcglynn/pixelmatch-cpp17.git",
        True,
    ),
}

# These don't map cleanly from MODULE.bazel and keep their hardcoded values.
# - absl: not a direct bazel_dep (pulled in transitively) so no version in MODULE.bazel
# - entt: vendored as a git subtree under third_party/entt, so there's no
#   bazel_dep/git_repository block for gen_cmakelists.py to read the version
#   from. Keep this tag in sync with the `git subtree add/pull` command when
#   bumping entt. CMake users still FetchContent entt as before; only Bazel
#   uses the vendored tree.
_HARDCODED_FETCHCONTENT = {
    "absl": ("absl", "https://github.com/abseil/abseil-cpp.git", "20250512.0"),
    "entt": ("entt", "https://github.com/skypjack/entt.git", "v3.16.0"),
}


def _normalize_version(version: str, use_v_prefix: bool) -> str:
    """Normalize a MODULE.bazel version to a git tag.

    - Strip BCR-specific suffixes like '.bcr.1'
    - Add 'v' prefix if requested and not a commit hash
    """
    # Strip .bcr.N suffix (Bazel Central Registry revision)
    version = re.sub(r"\.bcr\.\d+$", "", version)

    # Commit hashes (40 hex chars) and tags that already start with 'v' pass through
    if re.fullmatch(r"[0-9a-f]{40}", version):
        return version
    if version.startswith("v") and re.match(r"^v\d", version):
        return version

    if use_v_prefix and re.match(r"^\d+\.\d+", version):
        return f"v{version}"
    return version


def get_fetchcontent_externals() -> List[Tuple[str, str, str]]:
    """Build the FetchContent externals list, pulling versions from MODULE.bazel
    where possible and falling back to hardcoded values otherwise.

    Returns list of (name, git_url, tag_or_commit).
    """
    module_versions = extract_versions_from_module_bazel()
    result: List[Tuple[str, str, str]] = []

    for module_name, (fc_name, git_url, use_v_prefix) in _MODULE_TO_FETCHCONTENT.items():
        version = module_versions.get(module_name)
        if version is None:
            print(f"WARNING: Could not find version for '{module_name}' in MODULE.bazel")
            continue

        tag = _normalize_version(version, use_v_prefix)
        result.append((fc_name, git_url, tag))

    # Add hardcoded entries
    for fc_name, git_url, tag in _HARDCODED_FETCHCONTENT.values():
        result.append((fc_name, git_url, tag))

    # Sort for deterministic output
    result.sort(key=lambda x: x[0])

    return result


#
# Helper functions
#


def _run_bazel(args: List[str]) -> str:
    """Run a Bazel command and return stdout (stripped)."""
    try:
        return subprocess.check_output(
            BAZEL_PREFIX + args,
            text=True,
            stderr=subprocess.PIPE,
        ).strip()
    except subprocess.CalledProcessError as exc:
        cmd_str = " ".join(BAZEL_PREFIX + args)
        raise RuntimeError(f"Bazel command failed:\n  {cmd_str}\n{exc.stderr}") from exc


def query_targets() -> Dict[str, str]:
    """Return label->kind mapping for every cc_* and embed_resources target."""
    query = 'kind(".*cc_.*|embed_resources_generate_header", //donner/... + //examples/... + //third_party/...)'
    output = _run_bazel(["query", query, "--output=label_kind"])

    if not output:
        raise RuntimeError("No cc_library, cc_binary, cc_test, or embed_resources targets found.")

    results: Dict[str, str] = {}
    for line in output.splitlines():
        if not line.strip():
            continue

        parts = line.strip().split(" ")
        if len(parts) >= 3:  # Should have at least kind, 'rule', and label
            kind = parts[0]
            label = parts[2]

            # Normalize rule kinds
            if kind.endswith("cc_library"):
                kind = "cc_library"
                if label in results:
                    continue  # Already covered as an embed_resources target
            elif kind.endswith("cc_binary"):
                kind = "cc_binary"
            elif kind.endswith("cc_test"):
                kind = "cc_test"
            elif kind.endswith("embed_resources_generate_header"):
                label = label.removesuffix("_header_gen")
                kind = "embed_resources"
            else:
                print(f"Skipping unknown target kind: {kind} for label {label}")
                continue

            results[label] = kind
    return results


def query_labels(attr: str, target: str, *, relative_to: str) -> List[str]:
    """
    Return file paths listed in ``labels(attr, target)`` relative to *relative_to*.

    Only paths within the same Bazel package are returned; external labels are ignored.
    """
    pkg = target.split(":")[0].removeprefix("//")
    prefix = f"//{pkg}:"
    results: List[str] = []
    for line in _run_bazel(["query", f"labels({attr}, {target})"]).splitlines():
        line = line.strip()
        if line.startswith(prefix):
            label = line[len(prefix) :]
            results.append(str(Path(pkg, label).relative_to(relative_to)))
    return results

@dataclass
class CcTargetInfo:
    hdrs: List[str]
    srcs: List[str]
    copts: List[str]
    includes: List[str]

def get_cc_target_info(target_label: str) -> CcTargetInfo:
    """
    Return a CcTargetInfo containing headers, sources, copts, and includes
    for *target_label* using a single Bazel XML query.

    The paths in ``hdrs`` and ``srcs`` are made relative to the package
    directory to match the expectations of downstream CMake generation.
    """
    # Query Bazel once and parse the XML.  This is substantially faster than
    # issuing separate ``labels()`` queries for each attribute.
    xml_out = _run_bazel(["query", target_label, "--output=xml"])

    # The XML root looks like:
    # <query><rule ...> ... </rule></query>
    root = ElementTree.fromstring(xml_out)
    rule = root.find("rule")
    if rule is None:
        raise RuntimeError(f"Failed to parse XML for {target_label}")

    pkg = target_label.split(":", 1)[0].removeprefix("//")
    pkg_prefix = f"//{pkg}:"

    hdrs: List[str] = []
    srcs: List[str] = []
    copts: List[str] = []
    includes: List[str] = []

    def _maybe_add_label(value: str, out: List[str]) -> None:
        """Convert ``//pkg:sub/dir/file`` → ``sub/dir/file`` and append."""
        if value.startswith(pkg_prefix):
            rel = value[len(pkg_prefix):]
            out.append(str(Path(rel)))

    # Traverse <list name="..."> nodes to gather attributes.
    for lst in rule.findall("list"):
        name = lst.attrib.get("name", "")
        if name == "hdrs":
            for elem in lst.findall("label"):
                _maybe_add_label(elem.attrib["value"], hdrs)
        elif name == "srcs":
            for elem in lst.findall("label"):
                _maybe_add_label(elem.attrib["value"], srcs)
        elif name == "copts":
            for elem in lst.findall("string"):
                value = elem.attrib["value"]
                if value.startswith("-I") or value.startswith("-isystem"):
                    # Skip, these are handled in includes
                    continue

                copts.append(value)
        elif name == "includes":
            for elem in lst.findall("string"):
                includes.append(elem.attrib["value"])

    return CcTargetInfo(hdrs=hdrs, srcs=srcs, copts=copts, includes=includes)


def query_deps(target_label: str) -> List[str]:
    """Return cc_library deps, excluding *target_label* itself."""
    deps = _run_bazel(
        [
            "query",
            f'kind("cc_library", deps({target_label}, 2) - {target_label})',
        ]
    ).splitlines()

    return deps

def cmake_target_name(pkg: str, lib: str) -> str:
    """
    Convert a Bazel package / target name to a unique CMake target name.

    Examples
    --------
    >>> cmake_target_name("donner/svg", "svg_core")
    'donner_svg_svg_core'
    >>> cmake_target_name("", "donner")
    'donner'
    """
    pkg_rel = pkg.removeprefix("donner/").replace("/", "_")
    if not pkg_rel:  # root package
        return "donner" if lib == "donner" else f"donner_{lib}"

    base = f"donner_{pkg_rel}"
    return f"{base}_{lib}"


#
# CMake generation helpers
#

def write_library(f, name: str, srcs: List[str], hdrs: List[str]) -> None:
    """Emit a CMake library target (PUBLIC or INTERFACE) to file *f*."""
    if srcs:  # Concrete library
        f.write(f"add_library({name}\n")
        for path in srcs + hdrs:
            f.write(f"  {path}\n")
        f.write(")\n")
        f.write(f"target_include_directories({name} PUBLIC ${{PROJECT_SOURCE_DIR}})\n")
        f.write(
            f"set_target_properties({name} PROPERTIES CXX_STANDARD 20 "
            "CXX_STANDARD_REQUIRED YES POSITION_INDEPENDENT_CODE YES)\n"
        )
        f.write(f"target_compile_options({name} PRIVATE -fno-exceptions)\n")
    else:  # Header-only
        f.write(f"add_library({name} INTERFACE)\n")
        if hdrs:
            f.write(f"target_sources({name} INTERFACE\n")
            for p in hdrs:
                f.write(f"  {p}\n")
            f.write(")\n")
        f.write(f"target_include_directories({name} INTERFACE ${{PROJECT_SOURCE_DIR}})\n")
        f.write(f"target_compile_options({name} INTERFACE -fno-exceptions)\n")


#
# Step 1: top-level CMakeLists.txt
#


@dataclass
class EmbedInfo:
    package: str
    name: str
    header_output: str
    resources: Dict[str, str]

def get_embed_info(target_label: str) -> EmbedInfo:
    """
    Extract embed_resources information from a Bazel target label.

    Returns an EmbedInfo object containing the package, name, header output,
    and resources dictionary.
    """
    pkg, main_name = target_label.removeprefix("//").split(":", 1)

    repro_name = main_name + "_repro_json"
    repro_label = f"//{pkg}:{repro_name}"

    # Extract header_output and resources (from the repro json output).
    _run_bazel(["build", repro_label])

    repro_filename = _run_bazel(
        [
            "cquery",
            repro_label,
            "--output=files",
        ]
    ).strip().strip('"')

    if not repro_filename:
        raise RuntimeError(
            f"Could not determine header_output for {repro_label}"
        )

    # Read the repro file to extract the header_output and resources
    try:
        with open(repro_filename, "r") as f:
            repro_data = json.load(f)

        header_output = repro_data["header_output"]
        resources = repro_data["resources"]

        return EmbedInfo(pkg, main_name, header_output, resources)
    except (FileNotFoundError, json.JSONDecodeError, KeyError) as e:
        raise RuntimeError(f"Failed to parse repro file for //{pkg}:{main_name}: {e}")


def generate_root() -> None:
    """Create the project-root CMakeLists.txt."""
    path = Path("CMakeLists.txt")
    with path.open("w") as f:
        f.write("cmake_minimum_required(VERSION 3.20)\n")
        f.write("project(donner LANGUAGES C CXX)\n\n")
        f.write("set(CMAKE_CXX_STANDARD 20)\n")
        f.write("set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n")
        # Force static libraries — template specializations are spread across
        # libraries and resolved at binary link time (like Bazel).
        f.write("set(BUILD_SHARED_LIBS OFF CACHE BOOL \"\" FORCE)\n\n")
        f.write("include(FetchContent)\n")
        f.write("option(DONNER_BUILD_TESTS \"Build Donner tests\" OFF)\n\n")

        # ── Feature options (mirror Bazel flags) ───────────────────────
        f.write("# Feature options (mirror Bazel flags)\n")
        f.write("set(DONNER_RENDERER_BACKEND \"tiny_skia\" CACHE STRING\n")
        f.write("    \"Renderer backend: 'tiny_skia' (default) or 'skia'\")\n")
        f.write("option(DONNER_TEXT \"Enable text rendering (stb_truetype)\" ON)\n")
        f.write("option(DONNER_TEXT_FULL \"Enable full text rendering: FreeType + HarfBuzz\" OFF)\n")
        f.write("option(DONNER_TEXT_WOFF2 \"Enable WOFF2 font support (requires DONNER_TEXT)\" ON)\n")
        f.write("option(DONNER_FILTERS \"Enable SVG filter effects\" ON)\n\n")

        # Validation
        f.write("# Validate options\n")
        f.write("if(NOT DONNER_RENDERER_BACKEND STREQUAL \"skia\" AND NOT DONNER_RENDERER_BACKEND STREQUAL \"tiny_skia\")\n")
        f.write("  message(FATAL_ERROR \"DONNER_RENDERER_BACKEND must be 'skia' or 'tiny_skia', got '${DONNER_RENDERER_BACKEND}'\")\n")
        f.write("endif()\n")
        f.write("if(DONNER_TEXT_WOFF2 AND NOT DONNER_TEXT)\n")
        f.write("  message(FATAL_ERROR \"DONNER_TEXT_WOFF2 requires DONNER_TEXT to be ON\")\n")
        f.write("endif()\n")
        f.write("if(DONNER_TEXT_FULL AND NOT DONNER_TEXT)\n")
        f.write("  message(FATAL_ERROR \"DONNER_TEXT_FULL requires DONNER_TEXT to be ON\")\n")
        f.write("endif()\n\n")

        f.write("set(BUILD_GMOCK ON CACHE BOOL \"\" FORCE)\n\n")

        # External dependencies via FetchContent (versions from MODULE.bazel)
        externals = get_fetchcontent_externals()
        for name, repo, tag in externals:
            f.write(f"FetchContent_Declare(\n  {name}\n  GIT_REPOSITORY {repo}\n")
            f.write(f"  GIT_TAG        {tag}\n)\n")
            f.write(f"FetchContent_MakeAvailable({name})\n\n")

        # ── Skia (only when backend=skia) ──────────────────────────────
        f.write(f'if({_SKIA})\n')
        f.write("FetchContent_Declare(\n")
        f.write("  skia\n")
        f.write("  GIT_REPOSITORY https://github.com/google/skia.git\n")
        f.write("  GIT_TAG        d945cbcbbb5834245256e883803c2704f3a32e18\n")
        f.write(")\n")
        f.write("FetchContent_MakeAvailable(skia)\n")
        f.write(
            "execute_process(COMMAND python3 bin/fetch-gn "
            "WORKING_DIRECTORY ${skia_SOURCE_DIR})\n"
        )
        f.write(
            "execute_process(COMMAND python3 tools/git-sync-deps "
            "WORKING_DIRECTORY ${skia_SOURCE_DIR})\n"
        )
        f.write(
            "execute_process(\n"
            "  COMMAND ${skia_SOURCE_DIR}/bin/gn gen ${skia_SOURCE_DIR}/out/cmake\n"
            "    --ide=json\n"
            "    --json-ide-script=${skia_SOURCE_DIR}/gn/gn_to_cmake.py\n"
            "    \"--args=skia_use_gl=false skia_enable_tools=false\"\n"
            "  WORKING_DIRECTORY ${skia_SOURCE_DIR}\n"
            ")\n"
        )
        f.write("add_subdirectory(${skia_SOURCE_DIR}/out/cmake skia)\n")
        f.write("endif()\n\n")

        # Build / install rules for STB (header-only + impl)
        f.write("# STB libraries (locally vendored)\n")
        f.write(
            "add_library(stb_image third_party/stb/stb_image_impl.cc "
            "third_party/stb/stb_image.h)\n"
        )
        f.write(
            "target_include_directories(stb_image PUBLIC "
            "${PROJECT_SOURCE_DIR}/third_party)\n"
        )
        f.write(
            "set_target_properties(stb_image PROPERTIES CXX_STANDARD 20 "
            "CXX_STANDARD_REQUIRED YES POSITION_INDEPENDENT_CODE YES)\n"
        )

        f.write(
            "add_library(stb_image_write third_party/stb/stb_image_write_impl.cc "
            "third_party/stb/stb_image_write.h)\n"
        )
        f.write(
            "target_include_directories(stb_image_write PUBLIC "
            "${PROJECT_SOURCE_DIR}/third_party)\n"
        )
        f.write(
            "set_target_properties(stb_image_write PROPERTIES CXX_STANDARD 20 "
            "CXX_STANDARD_REQUIRED YES POSITION_INDEPENDENT_CODE YES)\n"
        )

        # ── stb_truetype (only when text is enabled) ───────────────────
        f.write("if(DONNER_TEXT)\n")
        f.write(
            "add_library(stb_truetype third_party/stb/stb_truetype_impl.cc "
            "third_party/stb/stb_truetype.h)\n"
        )
        f.write(
            "target_include_directories(stb_truetype PUBLIC "
            "${PROJECT_SOURCE_DIR}/third_party)\n"
        )
        f.write(
            "set_target_properties(stb_truetype PROPERTIES CXX_STANDARD 20 "
            "CXX_STANDARD_REQUIRED YES POSITION_INDEPENDENT_CODE YES)\n"
        )
        f.write("endif()\n")

        # ── tiny-skia-cpp (needed when backend=tiny_skia OR filters are on) ──
        f.write(f"if({_TINY_SKIA} OR DONNER_FILTERS)\n")
        f.write("# tiny-skia-cpp rendering backend\n")
        f.write("add_subdirectory(third_party/tiny-skia-cpp)\n")
        f.write("endif()\n\n")

        # ── brotli + woff2 (only when WOFF2 is enabled) ───────────────
        f.write("if(DONNER_TEXT_WOFF2)\n")
        f.write("# brotli compression (required by woff2)\n")
        f.write(
            "FetchContent_Declare(brotli\n"
            "  GIT_REPOSITORY https://github.com/google/brotli.git\n"
            "  GIT_TAG        v1.2.0\n"
            ")\n"
        )
        f.write("FetchContent_MakeAvailable(brotli)\n")
        # Satisfy woff2's find_package(BrotliDec) / find_package(BrotliEnc)
        f.write("set(BROTLIDEC_FOUND TRUE CACHE BOOL \"\" FORCE)\n")
        f.write("set(BROTLIDEC_INCLUDE_DIRS \"${brotli_SOURCE_DIR}/c/include\" CACHE PATH \"\" FORCE)\n")
        f.write("set(BROTLIDEC_LIBRARIES brotlidec CACHE STRING \"\" FORCE)\n")
        f.write("set(BROTLIENC_FOUND TRUE CACHE BOOL \"\" FORCE)\n")
        f.write("set(BROTLIENC_INCLUDE_DIRS \"${brotli_SOURCE_DIR}/c/include\" CACHE PATH \"\" FORCE)\n")
        f.write("set(BROTLIENC_LIBRARIES brotlienc CACHE STRING \"\" FORCE)\n")

        f.write("# woff2 font decoding\n")
        f.write("set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING \"\" FORCE)\n")
        module_versions = extract_versions_from_module_bazel()
        woff2_commit = module_versions.get("woff2", "1f184d05566b3e25827a1f8e68eb82b9ccf54f3b")
        f.write(
            "FetchContent_Declare(woff2\n"
            "  GIT_REPOSITORY https://github.com/google/woff2.git\n"
            f"  GIT_TAG        {woff2_commit}\n"
            ")\n"
        )
        f.write("FetchContent_MakeAvailable(woff2)\n")
        # woff2 uses include_directories() which isn't transitive; fix it.
        f.write("target_include_directories(woff2dec PUBLIC ${woff2_SOURCE_DIR}/include)\n")
        f.write("endif() # DONNER_TEXT_WOFF2\n\n")

        # ── FreeType + HarfBuzz (only when text_full is enabled) ──────────
        f.write(f"if({_TEXT_FULL})\n")
        f.write("find_package(PkgConfig REQUIRED)\n")
        f.write("pkg_check_modules(FREETYPE REQUIRED freetype2)\n")
        f.write("pkg_check_modules(HARFBUZZ REQUIRED harfbuzz)\n")
        f.write("endif() # DONNER_TEXT_FULL\n\n")

        # Optional test enable switch
        f.write("\n")
        f.write("if(DONNER_BUILD_TESTS)\n")
        f.write("  enable_testing()\n")
        f.write("endif()\n\n")

        # System font dependencies for Linux (Skia only)
        f.write(f"if({_SKIA})\n")
        f.write("if(UNIX AND NOT APPLE)\n")
        f.write("  find_package(PkgConfig REQUIRED)\n")
        f.write("  pkg_check_modules(FREETYPE REQUIRED freetype2)\n")
        f.write("  pkg_check_modules(FONTCONFIG REQUIRED fontconfig)\n")
        f.write("endif()\n")
        f.write("endif()\n\n")

        # Symlink hack for rules_cc runfiles
        f.write(
            "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink "
            "${rules_cc_SOURCE_DIR} ${CMAKE_BINARY_DIR}/rules_cc "
            "RESULT_VARIABLE _ignored)\n"
        )
        f.write(
            "add_library(rules_cc_runfiles "
            "${rules_cc_SOURCE_DIR}/cc/runfiles/runfiles.cc)\n"
        )
        f.write("target_include_directories(rules_cc_runfiles PUBLIC ${CMAKE_BINARY_DIR})\n\n")

        # Set up runfiles directory for CMake tests. Bazel tests use the runfiles
        # tree automatically, but CMake tests need RUNFILES_DIR pointing to the
        # source tree root (which already has donner/ in it). External repos need
        # symlinks at the source root to match the Bazel runfiles layout.
        f.write("# Runfiles setup for CMake tests\n")
        f.write("if(DONNER_BUILD_TESTS)\n")
        f.write("  # Symlink external repos to match Bazel runfiles layout\n")
        f.write("  if(NOT EXISTS ${PROJECT_SOURCE_DIR}/css-parsing-tests)\n")
        f.write("    execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink\n")
        f.write("      ${PROJECT_SOURCE_DIR}/third_party/css-parsing-tests\n")
        f.write("      ${PROJECT_SOURCE_DIR}/css-parsing-tests)\n")
        f.write("  endif()\n")
        f.write("endif()\n\n")

        # Python3 is needed for embed_resources custom commands.
        f.write("find_package(Python3 REQUIRED)\n\n")

        # Add generated subdirectories.
        #
        # Discover every internal Bazel package that contains at least one C++
        # target and emit a corresponding add_subdirectory() line.  This keeps
        # the root CMakeLists.txt in sync with the Bazel graph instead of
        # relying on a hand‑maintained list.
        discovered_pkgs = {
            label.removeprefix("//").split(":", 1)[0]
            for label in query_targets().keys()
        }

        # Skip third‑party packages that are handled manually elsewhere.
        discovered_pkgs -= SKIPPED_PACKAGES


        for pkg in sorted(discovered_pkgs):
            f.write(f"add_subdirectory({pkg})\n")
        f.write("\n")


def _sanitize(name: str) -> str:
    return "".join(ch if ch.isalnum() else "_" for ch in name)


def _emit_embed_resources(f, pkg: str, info: EmbedInfo) -> None:
    var_prefix = _sanitize(info.name).upper()
    out_dir = f"${{{var_prefix}_OUT}}"
    f.write(f"# embed_resources({info.name})\n")
    # 1. Variables and directories.
    for var, src in info.resources.items():
        f.write(f'set({var.upper()} ${{PROJECT_SOURCE_DIR}}/{pkg}/{src})\n')
    f.write(f'set({var_prefix}_INCLUDE_DIR ${{CMAKE_CURRENT_BINARY_DIR}}/{info.name})\n')
    f.write(
        f'set({var_prefix}_OUT ${{CMAKE_CURRENT_BINARY_DIR}}/{info.name}/embed_resources)\n'
    )
    f.write(f"file(MAKE_DIRECTORY {out_dir})\n")

    # 2. Custom command that runs tools/embed_resources.py.
    outputs = [
        f"{out_dir}/{_sanitize(Path(src).name)}.cpp" for src in info.resources.values()
    ]
    outputs.append(f"{out_dir}/{Path(info.header_output).name}")

    cmd = (
        f"${{Python3_EXECUTABLE}} ${{PROJECT_SOURCE_DIR}}/tools/embed_resources.py "
        f"--out {out_dir} --header {Path(info.header_output).name} "
        + " ".join(f"{k}=${{{k.upper()}}}" for k in info.resources)
    )

    f.write("add_custom_command(\n")
    f.write("  OUTPUT " + " ".join(outputs) + "\n")
    f.write(f"  COMMAND {cmd}\n")
    f.write(
        "  DEPENDS "
        + " ".join(f"${{{k.upper()}}}" for k in info.resources)
        + " ${PROJECT_SOURCE_DIR}/tools/embed_resources.py\n"
    )
    f.write(f'  COMMENT "Embedding {info.name}"\n  VERBATIM)\n')

    # 3. Object library that other targets can link against.
    tgt_name = cmake_target_name(pkg, info.name)
    f.write(f"add_library({tgt_name} {' '.join(outputs[:-1])})\n")
    f.write(
        f"target_include_directories({tgt_name} PUBLIC ${{{var_prefix}_INCLUDE_DIR}})\n"
    )
    f.write(
        f"set_target_properties({tgt_name} PROPERTIES "
        "CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES POSITION_INDEPENDENT_CODE YES)\n"
    )
    f.write("target_compile_options(" + tgt_name + " PRIVATE -fno-exceptions)\n\n")

#
# Step 2: Generate per-package CMakeLists.txt
#


def generate_all_packages() -> None:
    """Emit a CMakeLists.txt for every internal package discovered with Bazel."""

    print("Discovering cc_library, cc_binary, and cc_test targets...")
    by_pkg: DefaultDict[str, List[Tuple[str, str]]] = DefaultDict(list)
    for label, kind in query_targets().items():
        pkg, tgt = label.removeprefix("//").split(":", 1)
        if pkg in SKIPPED_PACKAGES:
            continue
        by_pkg[pkg].append((kind, tgt))

    # Per-package generation
    for pkg, entries in by_pkg.items():
        cmake = Path(pkg) / "CMakeLists.txt"
        cmake.parent.mkdir(parents=True, exist_ok=True)
        with cmake.open("w") as f:
            f.write("##\n")
            f.write("## Generated by tools/cmake/gen_cmakelists.py - DO NOT EDIT\n")
            f.write("##\n\n")
            f.write("cmake_minimum_required(VERSION 3.20)\n\n")

            for kind, tgt in sorted(entries):
                bazel_label = f"//{pkg}:{tgt}"
                cmake_name = cmake_target_name(pkg, tgt)

                if "_fuzzer" in tgt:
                    # Skip fuzzers, they are not built with CMake
                    print(f"Skipping fuzzer {bazel_label}")
                    continue

                if bazel_label in SKIPPED_TARGETS:
                    print(f"Skipping target {bazel_label}")
                    continue

                if kind == "embed_resources":
                    print(
                        "Adding target:",
                        cmake_name,
                        f" (kind={kind})",
                    )

                    embed_info = get_embed_info(bazel_label)
                    _emit_embed_resources(f, pkg, embed_info)
                    continue

                target_info = get_cc_target_info(bazel_label)

                hdrs = target_info.hdrs
                srcs = target_info.srcs
                copts = target_info.copts
                includes = target_info.includes

                # Check for conditional sources
                cond_srcs: Dict[str, str] = CONDITIONAL_SOURCES.get(cmake_name, {})
                extracted_cond_srcs: List[Tuple[str, str]] = []  # (filename, condition)
                if cond_srcs:
                    new_srcs = []
                    for s in srcs:
                        basename = Path(s).name
                        if basename in cond_srcs:
                            extracted_cond_srcs.append((s, cond_srcs[basename]))
                        else:
                            new_srcs.append(s)
                    srcs = new_srcs

                # Check if this target is conditional
                condition = CONDITIONAL_TARGETS.get(cmake_name)
                if condition:
                    f.write(f"if({condition})\n")

                print(
                    "Adding target:",
                    cmake_name,
                    f" (kind={kind}, srcs={len(srcs)}, hdrs={len(hdrs)})"
                    + (f" [conditional: {condition}]" if condition else ""),
                )

                scope = (
                    "PRIVATE"
                    if kind in {"cc_binary", "cc_test"}
                    else (
                        "LINK_PUBLIC" if srcs
                        else "INTERFACE"  # Header-only libraries
                    )
                )


                # If a target has conditional sources but no fixed sources, it
                # must be created as a concrete (STATIC) library rather than
                # INTERFACE, so that target_sources(PRIVATE ...) works.
                has_concrete_sources = bool(srcs) or bool(extracted_cond_srcs)
                if not srcs and extracted_cond_srcs:
                    scope = "LINK_PUBLIC"

                # Target declaration
                if kind == "cc_library":
                    if not srcs and extracted_cond_srcs:
                        # Create concrete library with just headers; sources
                        # will be added via target_sources() below.
                        f.write(f"add_library({cmake_name}\n")
                        for path in hdrs:
                            f.write(f"  {path}\n")
                        f.write(")\n")
                        f.write(f"target_include_directories({cmake_name} PUBLIC ${{PROJECT_SOURCE_DIR}})\n")
                        f.write(
                            f"set_target_properties({cmake_name} PROPERTIES CXX_STANDARD 20 "
                            "CXX_STANDARD_REQUIRED YES POSITION_INDEPENDENT_CODE YES)\n"
                        )
                        f.write(f"target_compile_options({cmake_name} PRIVATE -fno-exceptions)\n")
                    else:
                        write_library(f, cmake_name, srcs, hdrs)
                    if copts:
                        f.write(
                            f"target_compile_options({cmake_name} {scope} {' '.join(copts)})\n"
                        )
                    if includes:
                        include_scope = (
                            "PUBLIC" if srcs
                            else "INTERFACE"
                        )

                        for inc in includes:
                            f.write(
                                f"target_include_directories({cmake_name} {include_scope} "
                                f'"${{PROJECT_SOURCE_DIR}}/{pkg}/{inc}")\n'
                            )
                else:  # cc_binary or cc_test
                    f.write(f"add_executable({cmake_name}\n")
                    for p in srcs + hdrs:
                        f.write(f"  {p}\n")
                    f.write(")\n")
                    f.write(
                        f"target_include_directories({cmake_name} {scope} "
                        "${PROJECT_SOURCE_DIR})\n"
                    )
                    f.write(
                        f"set_target_properties({cmake_name} PROPERTIES "
                        "CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)\n"
                    )
                    flag = (
                        "-fexceptions"
                        if "_with_exceptions" in cmake_name
                        else "-fno-exceptions"
                    )
                    all_copts = [flag] + copts
                    f.write(
                        f"target_compile_options({cmake_name} {scope} {' '.join(all_copts)})\n"
                    )
                    if kind == "cc_test":
                        f.write(f"add_test(NAME {cmake_name} COMMAND {cmake_name})\n")
                        f.write(
                            f"set_tests_properties({cmake_name} PROPERTIES\n"
                            f'  ENVIRONMENT "RUNFILES_DIR=${{PROJECT_SOURCE_DIR}}")\n'
                        )
                    if includes:
                        for inc in includes:
                            f.write(
                                f"target_include_directories({cmake_name} {scope} "
                                f'"${{PROJECT_SOURCE_DIR}}/{pkg}/{inc}")\n'
                            )

                # Emit conditional sources via target_sources()
                for cond_src, cond in extracted_cond_srcs:
                    f.write(f"if({cond})\n")
                    f.write(f"  target_sources({cmake_name} PRIVATE {cond_src})\n")
                    f.write("endif()\n")

                # Link dependencies — split into unconditional and optional
                #
                # Backend-specific deps are stripped from most targets because
                # the `renderer` target pulls in the correct backend
                # conditionally.  For skia-only targets we keep skia deps but
                # strip tiny-skia deps (and vice-versa).
                _SKIA_DEPS = {
                    "donner_svg_renderer_renderer_skia",
                    "donner_svg_renderer_skia_deps",
                    "donner_svg_renderer_skia_deps_opt",
                    "donner_svg_renderer_skia_deps_unconfigured",
                }
                _TINY_SKIA_DEPS = {
                    "donner_svg_renderer_renderer_tiny_skia",
                    "donner_svg_renderer_filter_graph_executor",
                    "donner_svg_renderer_tiny_skia_deps",
                    "donner_svg_renderer_tiny_skia_filter_deps",
                    "tiny_skia",
                }
                _ALL_BACKEND_DEPS = _SKIA_DEPS | _TINY_SKIA_DEPS
                all_deps: List[str] = []
                for dep in query_deps(bazel_label):
                    if dep in SKIPPED_TARGETS:
                        continue
                    # KNOWN_BAZEL_TO_CMAKE_DEPS wins first so explicit
                    # mappings override the silently-dropped internal/abseil
                    # categories below.
                    mapped = KNOWN_BAZEL_TO_CMAKE_DEPS.get(dep)
                    if mapped:
                        if mapped != cmake_name:
                            all_deps.append(mapped)
                        continue
                    if _is_known_bazel_internal(dep):
                        continue
                    if dep.startswith("//"):
                        p, n = dep.removeprefix("//").split(":", 1)
                        mapped = cmake_target_name(p, n)
                    else:
                        msg = f"Unmapped external dep: {dep} (needed by {bazel_label})"
                        if msg not in _unmapped_deps:
                            _unmapped_deps.append(msg)
                            print(f"WARNING: {msg}")
                    if _should_skip_cmake_dep(mapped):
                        continue
                    if mapped and mapped != cmake_name:
                        all_deps.append(mapped)

                # Deduplicate preserving order
                all_deps = list(dict.fromkeys(all_deps))

                # Split deps into unconditional and optional. Optional deps
                # get if() guards using the condition from CONDITIONAL_TARGETS
                # (preferred over if(TARGET) to avoid ordering issues).
                fixed_deps = [d for d in all_deps if d not in OPTIONAL_DEPS]
                opt_deps = [d for d in all_deps if d in OPTIONAL_DEPS]

                if fixed_deps:
                    deps_list = " ".join(fixed_deps)
                    f.write(
                        f"target_link_libraries({cmake_name} {scope} {deps_list})\n"
                    )
                for opt_dep in opt_deps:
                    dep_cond = CONDITIONAL_TARGETS.get(opt_dep)
                    if dep_cond:
                        f.write(f"if({dep_cond})\n")
                    else:
                        f.write(f"if(TARGET {opt_dep})\n")
                    f.write(
                        f"  target_link_libraries({cmake_name} {scope} {opt_dep})\n"
                    )
                    f.write("endif()\n")

                # Emit conditional compile definitions
                for def_target, def_cond, defines, def_scope in CONDITIONAL_DEFINES:
                    if def_target == cmake_name:
                        f.write(f"if({def_cond})\n")
                        for d in defines:
                            f.write(
                                f"  target_compile_definitions({cmake_name} {def_scope} {d})\n"
                            )
                        f.write("endif()\n")

                # Hand-written tweaks
                if cmake_name == "donner_svg_renderer_tiny_skia_deps":
                    f.write(
                        f"target_link_libraries(donner_svg_renderer_tiny_skia_deps {scope} tiny_skia)\n"
                    )
                if cmake_name == "donner_svg_renderer_tiny_skia_filter_deps":
                    f.write(
                        f"target_link_libraries(donner_svg_renderer_tiny_skia_filter_deps {scope} tiny_skia)\n"
                    )
                if cmake_name == "donner_svg_renderer_skia_deps":
                    f.write(
                        f"target_link_libraries(donner_svg_renderer_skia_deps {scope} skia)\n"
                    )
                    f.write(
                        f"target_include_directories(donner_svg_renderer_skia_deps {scope} "
                        "${skia_SOURCE_DIR})\n"
                    )

                    # Use CMake platform detection instead of sys.platform so that
                    # the generated file is correct regardless of which OS runs the
                    # generator script.
                    f.write("if(APPLE)\n")
                    f.write(
                        f"  target_compile_definitions(donner_svg_renderer_skia_deps {scope} "
                        "DONNER_USE_CORETEXT)\n"
                    )
                    f.write("elseif(UNIX)\n")
                    f.write(
                        f"  target_compile_definitions(donner_svg_renderer_skia_deps {scope} "
                        "DONNER_USE_FREETYPE_WITH_FONTCONFIG)\n"
                    )
                    f.write(
                        f"  target_link_libraries(donner_svg_renderer_skia_deps {scope} "
                        "${FREETYPE_LIBRARIES} ${FONTCONFIG_LIBRARIES})\n"
                    )
                    f.write(
                        f"  target_include_directories(donner_svg_renderer_skia_deps {scope} "
                        "${FREETYPE_INCLUDE_DIRS} ${FONTCONFIG_INCLUDE_DIRS})\n"
                    )
                    f.write("else()\n")
                    f.write(
                        f"  target_compile_definitions(donner_svg_renderer_skia_deps {scope} "
                        "DONNER_USE_FREETYPE)\n"
                    )
                    f.write("endif()\n")

                # Extra link deps (e.g. system FreeType/HarfBuzz for text_backend_full)
                for extra_target, extra_cond, extra_libs in EXTRA_LINK_DEPS:
                    if extra_target == cmake_name:
                        f.write(f"target_link_libraries({cmake_name} PUBLIC {extra_libs})\n")

                for extra_target, extra_cond, extra_dirs in EXTRA_INCLUDE_DIRS:
                    if extra_target == cmake_name:
                        f.write(f"target_include_directories({cmake_name} PUBLIC {extra_dirs})\n")

                # Close conditional block
                if condition:
                    f.write(f"endif() # {condition}\n")

    # Umbrella INTERFACE target mirroring //:donner
    root = Path("CMakeLists.txt")
    with root.open("a") as f:
        f.write("\n# Umbrella library for external consumers\n")
        f.write("if(NOT TARGET donner)\n")
        f.write("  add_library(donner INTERFACE)\n")
        for dep in query_deps("//:donner"):
            mapped = KNOWN_BAZEL_TO_CMAKE_DEPS.get(dep)
            if not mapped:
                if _is_known_bazel_internal(dep):
                    continue
                if dep.startswith("//"):
                    mapped = cmake_target_name(*dep.removeprefix("//").split(":", 1))
            if _should_skip_cmake_dep(mapped):
                continue
            if mapped and mapped != "donner":
                if mapped in OPTIONAL_DEPS:
                    dep_cond = CONDITIONAL_TARGETS.get(mapped)
                    guard = dep_cond if dep_cond else f"TARGET {mapped}"
                    f.write(f"  if({guard})\n")
                    f.write(f"    target_link_libraries(donner INTERFACE {mapped})\n")
                    f.write(f"  endif()\n")
                else:
                    f.write(f"  target_link_libraries(donner INTERFACE {mapped})\n")
        f.write("endif()\n")

#
# Entry point
#


def _collect_cmake_files(root: Path) -> List[Path]:
    """Collect all CMakeLists.txt files under *root* (relative paths)."""
    return sorted(p.relative_to(root) for p in root.rglob("CMakeLists.txt"))


# Known targets that are provided by FetchContent, find_package, or hand-written
# parts of the generator (i.e., not defined by add_library in per-package files).
# The validator accepts references to these targets without flagging them.
_KNOWN_EXTERNAL_TARGETS: Set[str] = {
    # FetchContent
    "EnTT::EnTT",
    "gmock", "gmock_main", "gtest", "gtest_main",
    "nlohmann_json::nlohmann_json",
    "pixelmatch-cpp17",
    "zlib", "zlibstatic",
    "rules_cc_runfiles",
    # Abseil (pattern: absl::*)
    # Skia/tiny-skia backends
    "skia", "tiny_skia",
    # STB hand-written targets
    "stb_image", "stb_image_write", "stb_truetype",
    # Brotli / WOFF2
    "brotlidec", "brotlienc", "woff2dec",
    # System libraries
    "${FREETYPE_LIBRARIES}", "${HARFBUZZ_LIBRARIES}",
    "${FONTCONFIG_LIBRARIES}",
    # wgpu-native C++ wrapper. The package is Bazel-only (prebuilt archives
    # via http_archive that the CMake mirror doesn't fetch). References from
    # Geode-backend targets are wrapped in `if(TARGET ...)` via OPTIONAL_DEPS,
    # and the only callers that actually activate the Geode path are CMake-
    # excluded via CONDITIONAL_TARGETS. So at runtime this name never
    # resolves, but that's fine — no one links through to it.
    "donner_third_party_webgpu-cpp_webgpu_cpp",
    # Fallback umbrella
    "donner",
}


def _extract_cmake_targets_and_refs(
    root: Path,
    allowed_files: Optional[Set[Path]] = None,
) -> Tuple[Set[str], Dict[str, List[Tuple[str, Path]]], Dict[str, List[Tuple[str, Path]]]]:
    """Parse all CMakeLists.txt files under *root* and extract:
    - defined_targets: set of target names defined via add_library/add_executable
    - linked_targets: dict of target -> list of (referenced_target, file_path) from target_link_libraries
    - source_refs: dict of target -> list of (source_file, cmake_file_path) from add_library/add_executable/target_sources
    """
    defined: Set[str] = set()
    linked: Dict[str, List[Tuple[str, Path]]] = {}
    sources: Dict[str, List[Tuple[str, Path]]] = {}

    # Patterns to match CMake commands. Intentionally conservative.
    add_lib_re = re.compile(
        r"add_(?:library|executable)\s*\(\s*([A-Za-z0-9_:-]+)(?:\s+(?:STATIC|SHARED|MODULE|INTERFACE|OBJECT))?\s*([^)]*)\)",
        re.DOTALL,
    )
    tgt_sources_re = re.compile(
        r"target_sources\s*\(\s*([A-Za-z0-9_:-]+)\s+(?:PRIVATE|PUBLIC|INTERFACE)\s+([^)]*)\)",
        re.DOTALL,
    )
    tgt_link_re = re.compile(
        r"target_link_libraries\s*\(\s*([A-Za-z0-9_:-]+)\s+(?:PRIVATE|PUBLIC|INTERFACE|LINK_PUBLIC|LINK_PRIVATE)\s+([^)]*)\)",
        re.DOTALL,
    )

    for cmake_file in root.rglob("CMakeLists.txt"):
        if allowed_files is not None:
            if cmake_file.relative_to(root) not in allowed_files:
                continue
        text = cmake_file.read_text()

        # Strip comment lines so they don't interfere with regex matching.
        text = re.sub(r"#[^\n]*", "", text)

        for m in add_lib_re.finditer(text):
            name = m.group(1)
            defined.add(name)
            body = m.group(2).strip()
            srcs_list = sources.setdefault(name, [])
            for tok in body.split():
                tok = tok.strip()
                if not tok:
                    continue
                # Skip CMake keywords
                if tok in ("INTERFACE", "STATIC", "SHARED", "OBJECT", "PUBLIC", "PRIVATE"):
                    continue
                # Skip generator expressions and variable refs
                if tok.startswith("$") or tok.startswith("${"):
                    continue
                srcs_list.append((tok, cmake_file))

        for m in tgt_sources_re.finditer(text):
            name = m.group(1)
            body = m.group(2).strip()
            srcs_list = sources.setdefault(name, [])
            for tok in body.split():
                tok = tok.strip()
                if not tok or tok.startswith("$"):
                    continue
                srcs_list.append((tok, cmake_file))

        for m in tgt_link_re.finditer(text):
            name = m.group(1)
            body = m.group(2).strip()
            refs = linked.setdefault(name, [])
            for tok in body.split():
                tok = tok.strip()
                if not tok or tok.startswith("$"):
                    continue
                refs.append((tok, cmake_file))

    return defined, linked, sources


def _validate_generated_output(gen_root: Path, workspace: Path, generated_files: Set[Path]) -> List[str]:
    """Statically validate the generated CMakeLists.txt files.

    Only the files in *generated_files* (relative paths) are validated — this
    excludes vendored/untouched third-party CMakeLists.txt files.

    Checks:
    - Every source file referenced in add_library/add_executable/target_sources exists.
    - Every target referenced in target_link_libraries is either defined or known external.

    Returns a list of human-readable error messages. Empty = valid.
    """
    errors: List[str] = []
    defined, linked, sources = _extract_cmake_targets_and_refs(gen_root, allowed_files=generated_files)

    # Combine defined targets with known external targets for linkage validation.
    all_targets = defined | _KNOWN_EXTERNAL_TARGETS

    # 1. Check source files exist (resolved relative to the CMakeLists.txt file's directory)
    for target, refs in sources.items():
        for src_ref, cmake_file in refs:
            # Source files are relative to the containing CMakeLists.txt's dir
            # but resolved against the workspace (since the gen temp dir symlinks to workspace)
            cmake_dir_rel = cmake_file.parent.relative_to(gen_root)
            # For the root CMakeLists.txt, sources are relative to workspace root.
            candidate = workspace / cmake_dir_rel / src_ref
            if not candidate.exists():
                errors.append(
                    f"{cmake_file.relative_to(gen_root)}: target '{target}' "
                    f"references missing source '{src_ref}' (expected at {candidate})"
                )

    # 2. Check linked targets are defined
    for target, refs in linked.items():
        for ref_target, cmake_file in refs:
            # Allow absl::* pattern and anything matching known externals
            if ref_target.startswith("absl::"):
                continue
            if ref_target in all_targets:
                continue
            errors.append(
                f"{cmake_file.relative_to(gen_root)}: target '{target}' links "
                f"against undefined target '{ref_target}'"
            )

    return errors


def main() -> None:
    """Main entry point to generate all CMakeLists.txt files."""
    global _check_mode

    parser = argparse.ArgumentParser(
        description="Generate CMakeLists.txt files for Donner from Bazel targets."
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help=("Generate CMakeLists.txt files and statically validate the output. "
              "Exits 1 if any referenced source is missing, any target is undefined, "
              "or any external dep is unmapped. Does not modify the workspace."),
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Write generated files to this directory instead of the workspace.",
    )
    args = parser.parse_args()
    _check_mode = args.check

    if args.check:
        # Generate in-place to the workspace, capturing original contents first.
        # Since CMakeLists.txt files are gitignored, we snapshot them and restore
        # after validation so the workspace is unchanged on exit.
        workspace = Path.cwd()

        # Vendored third-party dirs that ship with their own CMakeLists.txt and
        # are NOT touched by gen_cmakelists.py. These are excluded from both
        # cleanup and validation.
        vendored_prefixes = (
            "bazel-",
            "third_party/tiny-skia-cpp",
            "third_party/stb",
            "third_party/frozen",
            "third_party/skia_user_config",
            "third_party/css-parsing-tests",
        )

        def _is_generated_cmake(rel: Path) -> bool:
            """Return True if this CMakeLists.txt is part of the generated output."""
            return not any(str(rel).startswith(s) for s in vendored_prefixes)

        # Snapshot existing files so we can restore the workspace after validation
        existing_files: Dict[Path, bytes] = {}
        for p in workspace.rglob("CMakeLists.txt"):
            rel = p.relative_to(workspace)
            if _is_generated_cmake(rel):
                existing_files[rel] = p.read_bytes()

        # Delete existing (potentially stale) generated CMakeLists.txt files before
        # regeneration, so the fresh output isn't contaminated by leftover files
        # whose corresponding targets no longer exist.
        for rel in existing_files:
            (workspace / rel).unlink()

        print("Generating and validating CMakeLists.txt files...")
        errors: List[str] = []
        try:
            generate_root()
            generate_all_packages()

            # Track which files were actually generated (so the validator only
            # scans generator output, not vendored third-party files).
            generated_set: Set[Path] = set()
            for p in workspace.rglob("CMakeLists.txt"):
                rel = p.relative_to(workspace)
                if _is_generated_cmake(rel):
                    generated_set.add(rel)

            # Statically validate the generated output
            errors = _validate_generated_output(workspace, workspace, generated_set)
        finally:
            # Remove anything newly created then restore originals
            for p in list(workspace.rglob("CMakeLists.txt")):
                rel = p.relative_to(workspace)
                if _is_generated_cmake(rel) and rel not in existing_files:
                    p.unlink()
            for rel, content in existing_files.items():
                p = workspace / rel
                p.write_bytes(content)

        had_errors = bool(errors) or bool(_unmapped_deps)
        if errors:
            print(f"\n{'='*60}")
            print(f"CMakeLists.txt VALIDATION FAILED ({len(errors)} error(s))")
            print(f"{'='*60}")
            for e in errors:
                print(f"  {e}")
        if _unmapped_deps:
            print(f"\nUnmapped external dependencies ({len(_unmapped_deps)}):")
            for msg in _unmapped_deps:
                print(f"  {msg}")
            print(
                "\nAdd these to KNOWN_BAZEL_TO_CMAKE_DEPS or _IGNORED_EXTERNAL_DEPS "
                "in tools/cmake/gen_cmakelists.py"
            )

        if had_errors:
            sys.exit(1)
        print("CMakeLists.txt validation passed.")
        sys.exit(0)
    else:
        output_dir = args.output_dir
        if output_dir:
            output_dir.mkdir(parents=True, exist_ok=True)
            os.chdir(output_dir)

        print("Generating CMakeLists.txt files for Donner libraries...")
        print("This may take a while, please wait...\n")

        generate_root()
        generate_all_packages()

        if _unmapped_deps:
            print(f"\nWARNING: Unmapped external dependencies:")
            for msg in _unmapped_deps:
                print(f"  {msg}")

        print("\nCMakeLists.txt generation complete.")
        print("You can now build Donner with CMake as follows:")
        print("  cmake -S . -B build && cmake --build build -j$(nproc)")
        print("\nOptions:")
        print("  -DDONNER_RENDERER_BACKEND=tiny_skia  (default)")
        print("  -DDONNER_RENDERER_BACKEND=skia")
        print("  -DDONNER_TEXT=OFF                     Disable text rendering")
        print("  -DDONNER_TEXT_FULL=ON                 Enable FreeType + HarfBuzz shaping")
        print("  -DDONNER_TEXT_WOFF2=OFF               Disable WOFF2 support")
        print("  -DDONNER_FILTERS=OFF                  Disable filter effects")


if __name__ == "__main__":
    main()
