#!/usr/bin/env python3
"""
Generate CMakeLists.txt files for Donner libraries using Bazel query.

This script performs three high-level steps:

1.  **generate_root()**  
    Creates the project-level `CMakeLists.txt`, declares external
    dependencies via FetchContent (absl, EnTT, frozen, googletest, …),
    embeds Skia, and wires up umbrella and convenience libraries.

2.  **generate_public_sans()**  
    Emits a tiny CMake fragment that runs the Donner
    `embed_resources.py` tool to compile the Public Sans font into a
    self-contained object file.

3.  **generate_all_packages()**  
    Discovers every `cc_library`, `cc_binary`, and `cc_test` under the
    `//…` Bazel workspace (excluding a few hand-curated packages) and
    mirrors them as CMake targets with appropriate source files,
    include paths, and transitive dependencies.

The generated tree lets consumers build Donner without Bazel, while
retaining the original dependency graph.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path
from typing import DefaultDict, Dict, List, Tuple

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
    "@frozen//:frozen": "frozen",
    "@nlohmann_json//:json": "nlohmann_json::nlohmann_json",
    "@rules_cc//cc/runfiles:runfiles": "rules_cc_runfiles",
    "@stb//:image_write": "stb_image_write",
    "@stb//:image": "stb_image",
    "@pixelmatch-cpp17//:pixelmatch-cpp17": "pixelmatch-cpp17",
}

# Packages whose CMake build is provided manually or by FetchContent and must
# *not* be auto-generated here.
SKIPPED_PACKAGES = {
    "",  # root package – handled by generate_root()
    "third_party/frozen",
    "third_party/stb",
    "pixelmatch-cpp17",
}

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


def query_cc_targets() -> List[Tuple[str, str]]:
    """Return ``(kind, label)`` for every cc_* target under //… (excluding externals)."""
    query = "kind(\".*cc_.*\", set(//donner/... //examples/...))"
    output = _run_bazel(["cquery", query, "--output=label_kind"])

    if not output:
        raise RuntimeError("No cc_library, cc_binary, or cc_test targets found.")

    results: List[Tuple[str, str]] = []
    for line in output.splitlines():
        if not line.strip():
            continue

        print(f"Processing line: {line}")
            
        parts = line.strip().split(" ")
        if len(parts) >= 2:  # Should have at least kind and label
            kind = parts[0]
            label = parts[2]  # The label is the third part
            
            # Normalize rule kinds
            if kind.endswith("cc_library"):
                kind = "cc_library"
            elif kind.endswith("cc_binary"):
                kind = "cc_binary"
            elif kind.endswith("cc_test"):
                kind = "cc_test"
            else:
                print(f"Skipping unknown target kind: {kind} for label {label}")
                continue
                
            results.append((kind, label))
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

def get_hdrs_and_srcs(target_label: str) -> Tuple[List[str], List[str]]:
    """Return hdrs and srcs for a given target, with paths relative to the package dir."""

    try:
        hdrs = query_labels("hdrs", target_label, relative_to=target_label.split(":")[0].removeprefix("//"))
        srcs = query_labels("srcs", target_label, relative_to=target_label.split(":")[0].removeprefix("//"))
        return hdrs, srcs
    except (RuntimeError, ValueError):
        raise RuntimeError(
            f"Failed to query headers and sources for target {target_label}. "
            "Ensure the target exists and has 'hdrs' or 'srcs' attributes."
        )

def get_copts(target_label: str) -> List[str]:
    """Return the copts for a given target."""
    try:
        return query_labels(
            "copts", target_label, relative_to=target_label.split(":")[0].removeprefix("//")
        )
    except RuntimeError:
        raise RuntimeError(
            f"Failed to query copts for target {target_label}. "
            "Ensure the target exists and has a 'copts' attribute."
        )

def get_includes(target_label: str) -> List[str]:
    """Return the includes for a given target."""
    try:
        return query_labels(
            "includes", target_label,
            relative_to=target_label.split(":")[0].removeprefix("//")
        )
    except RuntimeError:
        raise RuntimeError(
            f"Failed to query includes for target {target_label}. "
            "Ensure the target exists and has an 'includes' attribute."
        )


def query_deps(target_label: str) -> List[str]:
    """Return transitive cc_library dependencies for *target_label* (excluding itself)."""
    deps = _run_bazel(["query", f'kind("cc_library", deps({target_label}))']).splitlines()
    return [
        d.strip()
        for d in deps
        if d.strip() and d.strip() != target_label and (d.startswith("//") or d.startswith("@"))
    ]


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


def generate_root() -> None:
    """Create the project-root CMakeLists.txt."""
    path = Path("CMakeLists.txt")
    with path.open("w") as f:
        f.write("cmake_minimum_required(VERSION 3.20)\n")
        f.write("project(donner LANGUAGES CXX)\n\n")
        f.write("set(CMAKE_CXX_STANDARD 20)\n")
        f.write("set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n")
        f.write("include(FetchContent)\n")
        f.write("option(DONNER_BUILD_TESTS \"Build Donner tests\" OFF)\n\n")
        f.write("set(BUILD_GMOCK ON CACHE BOOL \"\" FORCE)\n\n")

        # External dependencies via FetchContent
        externals = [
            ("entt", "https://github.com/skypjack/entt.git", "v3.13.2"),
            ("googletest", "https://github.com/google/googletest.git", "v1.17.0"),
            ("nlohmann_json", "https://github.com/nlohmann/json.git", "v3.12.0"),
            ("absl", "https://github.com/abseil/abseil-cpp.git", "20250512.0"),
            ("rules_cc", "https://github.com/bazelbuild/rules_cc.git", "0.1.1"),
            ("pixelmatch-cpp17", "https://github.com/jwmcglynn/pixelmatch-cpp17.git", "ad7b103b746c9b23c61b4ce629fea64ae802df15"),
        ]
        for name, repo, tag in externals:
            f.write(f"FetchContent_Declare(\n  {name}\n  GIT_REPOSITORY {repo}\n")
            f.write(f"  GIT_TAG        {tag}\n)\n")
            f.write(f"FetchContent_MakeAvailable({name})\n\n")

        # Skia (large; requires GN-to-CMake conversion)
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

        f.write("\n# Frozen library (locally vendored)\n")
        f.write("add_library(frozen INTERFACE)\n")
        f.write(
            "target_include_directories(frozen INTERFACE "
            "${PROJECT_SOURCE_DIR}/third_party/frozen/include)\n"
        )

        # Optional test enable switch
        f.write("\n")
        f.write("if(DONNER_BUILD_TESTS)\n")
        f.write("  enable_testing()\n")
        f.write("endif()\n\n")

        f.write("# System font dependencies for Linux\n")
        f.write("if(UNIX AND NOT APPLE)\n")
        f.write("  find_package(PkgConfig REQUIRED)\n")
        f.write("  pkg_check_modules(FREETYPE REQUIRED freetype2)\n")
        f.write("  pkg_check_modules(FONTCONFIG REQUIRED fontconfig)\n")
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
            for _, label in query_cc_targets()
        }

        # Skip third‑party packages that are handled manually elsewhere, except
        # for Public Sans, whose CMakeLists.txt is generated in
        # generate_public_sans().
        discovered_pkgs -= SKIPPED_PACKAGES - {"third_party/public-sans"}

        # Ensure helper packages are always included.
        discovered_pkgs.add("third_party/public-sans")

        for pkg in sorted(discovered_pkgs):
            f.write(f"add_subdirectory({pkg})\n")
        f.write("\n")


#
# Step 2: Public Sans embedding helper
#


def generate_public_sans() -> None:
    """Create CMakeLists.txt under third_party/public-sans to embed the font."""
    cmake = Path("third_party/public-sans/CMakeLists.txt")
    with cmake.open("w") as f:
        f.write("cmake_minimum_required(VERSION 3.20)\n\n")
        f.write("##\n")
        f.write("## Generated by tools/cmake/gen_cmakelists.py – DO NOT EDIT\n")
        f.write("##\n\n")
        f.write("find_package(Python3 REQUIRED)\n")
        f.write(
            "set(PUBLIC_SANS_FONT "
            "${PROJECT_SOURCE_DIR}/third_party/public-sans/PublicSans-Medium.otf)\n"
        )
        f.write("set(PUBLIC_SANS_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/public-sans)\n")
        f.write("set(PUBLIC_SANS_OUT ${CMAKE_CURRENT_BINARY_DIR}/public-sans/embed_resources)\n")
        f.write("file(MAKE_DIRECTORY ${PUBLIC_SANS_OUT})\n")
        f.write("add_custom_command(\n")
        f.write(
            "  OUTPUT ${PUBLIC_SANS_OUT}/PublicSans_Medium_otf.cpp "
            "${PUBLIC_SANS_OUT}/PublicSansFont.h\n"
        )
        f.write(
            "  COMMAND ${Python3_EXECUTABLE} "
            "${PROJECT_SOURCE_DIR}/tools/embed_resources.py "
            "--out ${PUBLIC_SANS_OUT} --header PublicSansFont.h "
            "kPublicSansMediumOtf=${PUBLIC_SANS_FONT}\n"
        )
        f.write(
            "  DEPENDS ${PUBLIC_SANS_FONT} "
            "${PROJECT_SOURCE_DIR}/tools/embed_resources.py\n"
        )
        f.write("  COMMENT \"Embedding Public Sans\"\n")
        f.write("  VERBATIM\n")
        f.write(")\n")
        f.write(
            "add_library(donner_third_party_public-sans_public-sans "
            "${PUBLIC_SANS_OUT}/PublicSans_Medium_otf.cpp)\n"
        )
        f.write("target_include_directories(donner_third_party_public-sans_public-sans PUBLIC ${PUBLIC_SANS_INCLUDE_DIR})\n")
        f.write(
            "set_target_properties(donner_third_party_public-sans_public-sans PROPERTIES "
            "CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES POSITION_INDEPENDENT_CODE YES)\n"
        )
        f.write("target_compile_options(donner_third_party_public-sans_public-sans PRIVATE -fno-exceptions)\n")


#
# Step 3: Generate per-package CMakeLists.txt
#

def generate_all_packages() -> None:
    """Emit a CMakeLists.txt for every internal package discovered with Bazel."""
    
    print("Discovering cc_library, cc_binary, and cc_test targets...")
    by_pkg: DefaultDict[str, List[Tuple[str, str]]] = DefaultDict(list)
    for kind, label in query_cc_targets():
        pkg, tgt = label.removeprefix("//").split(":", 1)
        print(f"Processing {kind} {label} → {pkg}/{tgt}")
        if pkg in SKIPPED_PACKAGES:
            continue
        if "_fuzzer" in tgt:
            continue  # Skip fuzzers
        by_pkg[pkg].append((kind, tgt))

    # Per-package generation
    for pkg, entries in by_pkg.items():
        cmake = Path(pkg) / "CMakeLists.txt"
        cmake.parent.mkdir(parents=True, exist_ok=True)
        with cmake.open("w") as f:
            f.write("##\n")
            f.write("## Generated by tools/cmake/gen_cmakelists.py – DO NOT EDIT\n")
            f.write("##\n\n")
            f.write("cmake_minimum_required(VERSION 3.20)\n\n")

            for kind, tgt in sorted(entries):
                bazel_label = f"//{pkg}:{tgt}"
                hdrs, srcs = get_hdrs_and_srcs(bazel_label)
                cmake_name = cmake_target_name(pkg, tgt)
                copts = get_copts(bazel_label)
                includes = get_includes(bazel_label)

                scope = (
                    "PRIVATE"
                    if kind in {"cc_binary", "cc_test"}
                    else (
                        "LINK_PUBLIC" if srcs
                        else "INTERFACE"  # Header-only libraries
                    )
                )

                if "_fuzzer" in tgt:
                    # Skip fuzzers, they are not built with CMake
                    print(f"Skipping fuzzer {bazel_label}")
                    continue

                print("Adding target:", cmake_name,
                      f" (kind={kind}, srcs={len(srcs)}, hdrs={len(hdrs)})")

                # Target declaration
                if kind == "cc_library":
                    write_library(f, cmake_name, srcs, hdrs)
                    if copts:
                        f.write(
                            f"target_compile_options({cmake_name} {scope} {' '.join(copts)})\n"
                        )
                    if includes:
                        for inc in includes:
                            f.write(
                                f"target_include_directories({cmake_name} {scope} "
                                f"${{PROJECT_SOURCE_DIR}}/{pkg}/{inc})\n"
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
                        "-fexceptions" if "_with_exceptions" in cmake_name else "-fno-exceptions"
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
                                f"${{PROJECT_SOURCE_DIR}}/{pkg}/{inc})\n"
                            )

                # Link dependencies
                deps: List[str] = []
                for dep in query_deps(bazel_label):
                    mapped = KNOWN_BAZEL_TO_CMAKE_DEPS.get(dep)
                    if not mapped and dep.startswith("//"):
                        p, n = dep.removeprefix("//").split(":", 1)
                        mapped = cmake_target_name(p, n)
                    if mapped and mapped != cmake_name:
                        deps.append(mapped)

                if deps:
                    deps_list = " ".join(dict.fromkeys(deps))
                    f.write(f"target_link_libraries({cmake_name} {scope} {deps_list})\n")

                # Hand-written tweaks
                if cmake_name == "donner_svg_renderer_skia_deps":
                    f.write(f"target_link_libraries(donner_svg_renderer_skia_deps {scope} skia)\n")
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
                            f"target_compile_definitions(donner_svg_renderer_skia_deps {scope} DONNER_USE_FREETYPE_WITH_FONTCONFIG)\n"
                        )
                        f.write(
                            f"target_link_libraries(donner_svg_renderer_skia_deps {scope} ${{FREETYPE_LIBRARIES}} ${{FONTCONFIG_LIBRARIES}})\n"
                        )
                        f.write(
                            f"target_include_directories(donner_svg_renderer_skia_deps {scope} ${{FREETYPE_INCLUDE_DIRS}} ${{FONTCONFIG_INCLUDE_DIRS}})\n"
                        )
                    else:
                        f.write(
                            f"target_compile_definitions(donner_svg_renderer_skia_deps {scope} "
                            "DONNER_USE_FREETYPE)\n"
                        )

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
    generate_public_sans()
    generate_all_packages()

    print("\nCMakeLists.txt generation complete.")
    print("You can now build Donner with CMake as follows:")
    print("  cmake -S . -B build && cmake --build build -j$(nproc)")

if __name__ == "__main__":
    main()
