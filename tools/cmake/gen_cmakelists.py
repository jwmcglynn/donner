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

import subprocess
import sys
import re
from pathlib import Path
from typing import DefaultDict, Dict, List, Set, Tuple
from dataclasses import dataclass
import json
import os
import xml.etree.ElementTree as ElementTree

#
# Bazel helpers
#

# Use bzlmod-aware queries since this repository relies on MODULE.bazel.
BAZEL_PREFIX = ["bazel"]

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
    "@tiny-skia-cpp//src:tiny_skia_lib": "tiny_skia",
    "@tiny-skia-cpp//src:tiny_skia_lib_native": "tiny_skia",
    "@tiny-skia-cpp//src/tiny_skia:tiny_skia_core": "tiny_skia",
    "@tiny-skia-cpp//src/tiny_skia/filter:filter": "tiny_skia",
    "@woff2//:woff2_decode": "woff2dec",
}

# Packages whose CMake build is provided manually or by FetchContent and must
# *not* be auto-generated here.
SKIPPED_PACKAGES = {
    "",  # root package - handled by generate_root()
    "third_party/stb",
    "pixelmatch-cpp17",
    "donner/benchmarks",
}

# Individual targets to skip (require optional deps like HarfBuzz).
SKIPPED_TARGETS = set()

# Helper constants for CMake condition strings.
_SKIA = 'DONNER_RENDERER_BACKEND STREQUAL "skia"'
_TINY_SKIA = 'DONNER_RENDERER_BACKEND STREQUAL "tiny_skia"'

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
    "donner_svg_renderer_filter_graph_executor",
    "donner_svg_resources_font_manager",
    "donner_base_fonts_woff2_parser",
    "donner_svg_renderer_skia_deps",
    "donner_svg_renderer_skia_deps_opt",
    "donner_svg_renderer_skia_deps_unconfigured",
    "donner_third_party_skia_user_config_user_config",
    "donner_svg_renderer_tests_renderer_test_utils",
}

# Sources that must be conditionally compiled. Maps cmake target name → dict of
# source filename → CMake condition. Matched sources are removed from the main
# add_library() call and added via target_sources() inside if() blocks.
CONDITIONAL_SOURCES: Dict[str, Dict[str, str]] = {
    "donner_svg_renderer_renderer": {
        "RendererTinySkiaBackend.cc": _TINY_SKIA,
        "RendererSkiaBackend.cc": _SKIA,
    },
    "donner_svg_renderer_tests_renderer_test_backend": {
        "RendererTestBackendTinySkia.cc": _TINY_SKIA,
        "RendererTestBackendSkia.cc": _SKIA,
    },
}

# Compile definitions gated on CMake options.
# (cmake_target, condition, [definitions], scope)
CONDITIONAL_DEFINES: List[Tuple[str, str, List[str], str]] = [
    ("donner_svg_renderer_renderer_tiny_skia", "DONNER_FILTERS", ["DONNER_FILTERS_ENABLED"], "PUBLIC"),
    ("donner_svg_renderer_renderer_tiny_skia", "DONNER_TEXT", ["DONNER_TEXT_ENABLED"], "PUBLIC"),
    ("donner_svg_resources_font_manager", "DONNER_TEXT_WOFF2", ["DONNER_TEXT_WOFF2_ENABLED"], "PUBLIC"),
    ("donner_svg_resources_font_manager_tests", "DONNER_TEXT_WOFF2", ["DONNER_TEXT_WOFF2_ENABLED"], "PRIVATE"),
]

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
        f.write("option(DONNER_TEXT_WOFF2 \"Enable WOFF2 font support (requires DONNER_TEXT)\" ON)\n")
        f.write("option(DONNER_FILTERS \"Enable SVG filter effects\" ON)\n\n")

        # Validation
        f.write("# Validate options\n")
        f.write("if(NOT DONNER_RENDERER_BACKEND STREQUAL \"skia\" AND NOT DONNER_RENDERER_BACKEND STREQUAL \"tiny_skia\")\n")
        f.write("  message(FATAL_ERROR \"DONNER_RENDERER_BACKEND must be 'skia' or 'tiny_skia', got '${DONNER_RENDERER_BACKEND}'\")\n")
        f.write("endif()\n")
        f.write("if(DONNER_TEXT_WOFF2 AND NOT DONNER_TEXT)\n")
        f.write("  message(FATAL_ERROR \"DONNER_TEXT_WOFF2 requires DONNER_TEXT to be ON\")\n")
        f.write("endif()\n\n")

        f.write("set(BUILD_GMOCK ON CACHE BOOL \"\" FORCE)\n\n")

        # External dependencies via FetchContent
        externals = [
            ("entt", "https://github.com/skypjack/entt.git", "v3.13.2"),
            ("googletest", "https://github.com/google/googletest.git", "v1.17.0"),
            ("nlohmann_json", "https://github.com/nlohmann/json.git", "v3.12.0"),
            ("absl", "https://github.com/abseil/abseil-cpp.git", "20250512.0"),
            ("rules_cc", "https://github.com/bazelbuild/rules_cc.git", "0.1.1"),
            ("pixelmatch-cpp17", "https://github.com/jwmcglynn/pixelmatch-cpp17.git", "ad7b103b746c9b23c61b4ce629fea64ae802df15"),
            ("zlib", "https://github.com/madler/zlib.git", "v1.3.1"),
        ]
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
        f.write(
            "FetchContent_Declare(woff2\n"
            "  GIT_REPOSITORY https://github.com/google/woff2.git\n"
            "  GIT_TAG        1bccf208bca986e53a647dfe4811322adb06ecf8\n"
            ")\n"
        )
        f.write("FetchContent_MakeAvailable(woff2)\n")
        # woff2 uses include_directories() which isn't transitive; fix it.
        f.write("target_include_directories(woff2dec PUBLIC ${woff2_SOURCE_DIR}/include)\n")
        f.write("endif() # DONNER_TEXT_WOFF2\n\n")

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
                all_deps: List[str] = []
                for dep in query_deps(bazel_label):
                    if dep in SKIPPED_TARGETS:
                        continue
                    mapped = KNOWN_BAZEL_TO_CMAKE_DEPS.get(dep)
                    if not mapped and dep.startswith("//"):
                        p, n = dep.removeprefix("//").split(":", 1)
                        mapped = cmake_target_name(p, n)
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
                if cmake_name == "donner_svg_renderer_skia_deps":
                    f.write(
                        f"target_link_libraries(donner_svg_renderer_skia_deps {scope} skia)\n"
                    )
                    f.write(
                        f"target_include_directories(donner_svg_renderer_skia_deps {scope} "
                        "${skia_SOURCE_DIR})\n"
                    )

                    if sys.platform == "darwin":
                        f.write(
                            f"target_compile_definitions(donner_svg_renderer_skia_deps {scope} "
                            "DONNER_USE_CORETEXT)\n"
                        )
                    elif sys.platform.startswith("linux"):
                        f.write(
                            f"target_compile_definitions(donner_svg_renderer_skia_deps {scope} "
                            "DONNER_USE_FREETYPE_WITH_FONTCONFIG)\n"
                        )
                        f.write(
                            f"target_link_libraries(donner_svg_renderer_skia_deps {scope} "
                            "${FREETYPE_LIBRARIES} ${FONTCONFIG_LIBRARIES})\n"
                        )
                        f.write(
                            f"target_include_directories(donner_svg_renderer_skia_deps {scope} "
                            "${FREETYPE_INCLUDE_DIRS} ${FONTCONFIG_INCLUDE_DIRS})\n"
                        )
                    else:
                        f.write(
                            f"target_compile_definitions(donner_svg_renderer_skia_deps {scope} "
                            "DONNER_USE_FREETYPE)\n"
                        )

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
            mapped = (
                KNOWN_BAZEL_TO_CMAKE_DEPS.get(dep)
                if not dep.startswith("//")
                else cmake_target_name(*dep.removeprefix("//").split(":", 1))
            )
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


def main() -> None:
    """Main entry point to generate all CMakeLists.txt files."""
    print("Generating CMakeLists.txt files for Donner libraries...")
    print("This may take a while, please wait...\n")

    generate_root()
    generate_all_packages()

    print("\nCMakeLists.txt generation complete.")
    print("You can now build Donner with CMake as follows:")
    print("  cmake -S . -B build && cmake --build build -j$(nproc)")
    print("\nOptions:")
    print("  -DDONNER_RENDERER_BACKEND=tiny_skia  (default)")
    print("  -DDONNER_RENDERER_BACKEND=skia")
    print("  -DDONNER_TEXT=OFF                     Disable text rendering")
    print("  -DDONNER_TEXT_WOFF2=OFF               Disable WOFF2 support")
    print("  -DDONNER_FILTERS=OFF                  Disable filter effects")


if __name__ == "__main__":
    main()
