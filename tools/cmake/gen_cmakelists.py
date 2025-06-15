#!/usr/bin/env python3
"""Generate CMakeLists.txt files for Donner libraries using bazel query."""

from __future__ import annotations

import subprocess
from pathlib import Path
from typing import List, Tuple

BAZEL_QUERY_PREFIX = ["bazel", "query", "--enable_workspace", "--noenable_bzlmod"]


def query_labels(attr: str, target: str, *, relative_to: str) -> List[str]:
    expr = f"labels({attr}, {target})"
    out = subprocess.check_output(BAZEL_QUERY_PREFIX + [expr], text=True, stderr=subprocess.DEVNULL)
    pkg = target.split(":")[0].removeprefix("//")
    prefix = f"//{pkg}:"
    results = []
    for line in out.splitlines():
        if not line.startswith(prefix):
            continue
        label = line[len(prefix) :]
        full = Path(pkg, label)
        results.append(str(full.relative_to(relative_to)))
    return results


def query_cc_libraries(pkg: str) -> List[str]:
    expr = f"kind(cc_library, //{pkg}:*)"
    out = subprocess.check_output(
        BAZEL_QUERY_PREFIX + [expr], text=True, stderr=subprocess.DEVNULL
    )
    libs = []
    for line in out.splitlines():
        libs.append(line.split(":")[1])
    return libs


def cmake_target_name(pkg: str, lib: str) -> str:
    pkg_rel = pkg.removeprefix("donner/").replace("/", "_")
    base = f"donner_{pkg_rel}"
    pkg_last = pkg_rel.split("_")[-1]
    if lib == pkg_last:
        return base
    if lib.startswith(pkg_last + "_"):
        return f"{base}_{lib[len(pkg_last)+1:]}"
    if lib.endswith("_" + pkg_last):
        return f"{base}_{lib[:-len(pkg_last)-1]}"
    return f"{base}_{lib}"


def generate_root() -> None:
    path = Path("CMakeLists.txt")
    with path.open("w") as f:
        f.write("cmake_minimum_required(VERSION 3.20)\n")
        f.write("project(donner LANGUAGES CXX)\n\n")
        f.write("set(CMAKE_CXX_STANDARD 20)\n")
        f.write("set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n")
        f.write("include(FetchContent)\n\n")
        f.write("enable_testing()\n\n")

        f.write("FetchContent_Declare(\n")
        f.write("  entt\n")
        f.write("  GIT_REPOSITORY https://github.com/skypjack/entt.git\n")
        f.write("  GIT_TAG        v3.13.2\n")
        f.write(")\n")
        f.write("FetchContent_MakeAvailable(entt)\n\n")

        f.write("FetchContent_Declare(\n")
        f.write("  googletest\n")
        f.write("  GIT_REPOSITORY https://github.com/google/googletest.git\n")
        f.write("  GIT_TAG        v1.17.0\n")
        f.write(")\n")
        f.write("FetchContent_MakeAvailable(googletest)\n\n")

        f.write("FetchContent_Declare(\n")
        f.write("  nlohmann_json\n")
        f.write("  GIT_REPOSITORY https://github.com/nlohmann/json.git\n")
        f.write("  GIT_TAG        v3.12.0\n")
        f.write(")\n")
        f.write("FetchContent_MakeAvailable(nlohmann_json)\n\n")

        f.write("FetchContent_Declare(\n")
        f.write("  rules_cc\n")
        f.write("  GIT_REPOSITORY https://github.com/bazelbuild/rules_cc.git\n")
        f.write("  GIT_TAG        0.1.1\n")
        f.write(")\n")
        f.write("FetchContent_MakeAvailable(rules_cc)\n\n")

        f.write("execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink\n")
        f.write("  ${rules_cc_SOURCE_DIR} ${CMAKE_BINARY_DIR}/rules_cc\n")
        f.write("  RESULT_VARIABLE _ignored)\n")
        f.write(
            "add_library(rules_cc_runfiles ${rules_cc_SOURCE_DIR}/cc/runfiles/runfiles.cc)\n"
        )
        f.write("target_include_directories(rules_cc_runfiles PUBLIC ${CMAKE_BINARY_DIR})\n\n")

        f.write("add_subdirectory(donner/base)\n")
        f.write("add_subdirectory(third_party/frozen)\n")
        f.write("add_subdirectory(donner/css)\n")


def write_library(f, name: str, srcs: List[str], hdrs: List[str]) -> None:
    if srcs:
        f.write(f"add_library({name}\n")
        for path in srcs + hdrs:
            f.write(f"  {path}\n")
        f.write(")\n")
        f.write(
            f"target_include_directories({name} PUBLIC ${{PROJECT_SOURCE_DIR}})\n"
        )
        f.write(
            f"set_target_properties({name} PROPERTIES CXX_STANDARD 20 "
            "CXX_STANDARD_REQUIRED YES POSITION_INDEPENDENT_CODE YES)\n"
        )
        f.write(f"target_compile_options({name} PRIVATE -fno-exceptions)\n")
    else:
        f.write(f"add_library({name} INTERFACE)\n")
        if hdrs:
            f.write(f"target_sources({name} INTERFACE\n")
            for p in hdrs:
                f.write(f"  {p}\n")
            f.write(")\n")
        f.write(
            f"target_include_directories({name} INTERFACE ${{PROJECT_SOURCE_DIR}})\n"
        )
        f.write(f"target_compile_options({name} INTERFACE -fno-exceptions)\n")


def generate_base() -> None:
    pkg = Path("donner/base")
    libs = query_cc_libraries("donner/base")

    utils_hdrs = query_labels(
        "hdrs", "//donner/base:base_test_utils", relative_to="donner/base"
    )
    testdata = query_labels(
        "srcs", "//donner/base:base_tests_testdata", relative_to="donner/base"
    )
    tests = {
        "base_tests": query_labels(
            "srcs", "//donner/base:base_tests", relative_to="donner/base"
        ),
        "base_tests_ndebug": query_labels(
            "srcs", "//donner/base:base_tests_ndebug", relative_to="donner/base"
        ),
        "rcstring_tests_with_exceptions": query_labels(
            "srcs",
            "//donner/base:rcstring_tests_with_exceptions",
            relative_to="donner/base",
        ),
    }

    out = pkg / "CMakeLists.txt"
    with out.open("w") as f:
        f.write("##\n")
        f.write("## Generated by tools/cmake/gen_cmakelists.py.\n")
        f.write("## NOTE: Do not edit this file directly, edit gen_cmakelists.py instead\n")
        f.write("##\n\n")

        for lib in libs:
            if lib in {"base_utils_h_ndebug", "base_test_utils"}:
                continue
            srcs = query_labels("srcs", f"//donner/base:{lib}", relative_to="donner/base")
            hdrs = query_labels("hdrs", f"//donner/base:{lib}", relative_to="donner/base")
            target = cmake_target_name("donner/base", lib)
            write_library(f, target, srcs, hdrs)
            if lib == "base":
                f.write("target_link_libraries(donner_base PUBLIC EnTT::EnTT)\n")

        f.write("\nif(DONNER_BUILD_TESTS)\n")
        if utils_hdrs:
            f.write("  add_library(donner_base_test_utils INTERFACE\n")
            for p in utils_hdrs:
                f.write(f"    {p}\n")
            f.write("  )\n")
            f.write(
                "  target_include_directories(donner_base_test_utils INTERFACE "
                "${PROJECT_SOURCE_DIR})\n"
            )
            f.write(
                "  target_link_libraries(donner_base_test_utils INTERFACE gtest gmock "
                "rules_cc_runfiles)\n"
            )

        for name, src in tests.items():
            if not src:
                continue
            f.write(f"\n  add_executable({name}\n")
            
            for p in src:
                if p.endswith("Runfiles_tests.cc"):
                    # Skip the Runfiles tests, they do not currently work in CMake
                    continue
                f.write(f"    {p}\n")

            f.write("  )\n")
            if name == "base_tests_ndebug":
                f.write("  target_compile_definitions(base_tests_ndebug PRIVATE NDEBUG)\n")
            if name == "rcstring_tests_with_exceptions":
                f.write(
                    "  target_compile_options(rcstring_tests_with_exceptions "
                    "PRIVATE -fexceptions)\n"
                )
            else:
                f.write(f"  target_compile_options({name} PRIVATE -fno-exceptions)\n")
            f.write(f"  target_link_libraries({name} PRIVATE donner_base gtest_main gmock_main")
            if utils_hdrs:
                f.write(" donner_base_test_utils")
            f.write(")\n")
            f.write(f"  add_test(NAME {name} COMMAND {name})\n")
            f.write(
                f"  set_tests_properties({name} PROPERTIES ENVIRONMENT "
                f"\"RUNFILES_DIR=${{PROJECT_SOURCE_DIR}}\")\n"
            )

        if testdata:
            f.write("\n  file(COPY\n")
            for p in testdata:
                f.write(f"    {p}\n")
            f.write("  DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/tests/testdata)\n")
        f.write("endif()\n")


def generate_css() -> None:
    pkg = Path("donner/css")
    libs = []
    libs += [
        ("donner/css", name)
        for name in query_cc_libraries("donner/css")
        if name != "selector_test_utils"
    ]
    libs += [
        ("donner/css/parser", name)
        for name in query_cc_libraries("donner/css/parser")
    ]

    selector_utils_hdrs = query_labels(
        "hdrs", "//donner/css:selector_test_utils", relative_to="donner/css"
    )

    css_tests = query_labels("srcs", "//donner/css:css_tests", relative_to="donner/css")
    css_parser_tests = query_labels(
        "srcs", "//donner/css/parser:parser_tests", relative_to="donner/css"
    )
    css_parsing_tests = query_labels(
        "srcs", "//donner/css/parser:css_parsing_tests", relative_to="donner/css"
    )

    # TODO: Re-enable parsing tests, currently they are disabled since they require bazel runfiles
    # to be set up correctly, which is not the case in CMake.
    #css_parser_tests = None
    css_parsing_tests = None

    out = pkg / "CMakeLists.txt"
    with out.open("w") as f:
        f.write("##\n")
        f.write("## Generated by tools/cmake/gen_cmakelists.py.\n")
        f.write("## NOTE: Do not edit this file directly, edit gen_cmakelists.py instead\n")
        f.write("##\n\n")

        for p, lib in libs:
            srcs = query_labels("srcs", f"//{p}:{lib}", relative_to="donner/css")
            hdrs = query_labels("hdrs", f"//{p}:{lib}", relative_to="donner/css")
            target = cmake_target_name(p, lib)
            write_library(f, target, srcs, hdrs)
            if target == "donner_css_core":
                f.write(
                    "target_link_libraries(donner_css_core PUBLIC donner_base "
                    "donner_base_element donner_base_xml_qualified_name frozen)\n"
                )
            elif target == "donner_css_parser":
                f.write(
                    "target_link_libraries(donner_css_parser PUBLIC donner_css_core "
                    "donner_base donner_base_parser)\n"
                )
            elif target == "donner_css":
                f.write(
                    "target_link_libraries(donner_css PUBLIC donner_css_core donner_css_parser)\n"
                )

        f.write("\nif(DONNER_BUILD_TESTS)\n")
        if selector_utils_hdrs:
            f.write("  add_library(donner_css_selector_test_utils INTERFACE\n")
            for p in selector_utils_hdrs:
                f.write(f"    {p}\n")
            f.write("  )\n")
            f.write(
                "  target_compile_options(donner_css_selector_test_utils INTERFACE "
                "-fno-exceptions)\n"
            )
            f.write(
                "  target_include_directories(donner_css_selector_test_utils INTERFACE "
                "${PROJECT_SOURCE_DIR})\n"
            )
            f.write(
                "  target_link_libraries(donner_css_selector_test_utils INTERFACE gtest)\n"
            )

        if css_tests:
            f.write("\n  add_executable(css_tests\n")
            for p in css_tests:
                f.write(f"    {p}\n")
            f.write("  )\n")
            f.write("  target_compile_options(css_tests PRIVATE -fno-exceptions)\n")
            f.write(
                "  target_link_libraries(css_tests PRIVATE donner_css_parser "
                "donner_css_core donner_css_selector_test_utils gtest_main "
                "donner_base_test_utils donner_base_element_fake)\n"
            )
            f.write("  add_test(NAME css_tests COMMAND css_tests)\n")
            f.write(
                "  set_tests_properties(css_tests PROPERTIES ENVIRONMENT "
                "\"RUNFILES_DIR=${PROJECT_SOURCE_DIR}\")\n"
            )

        if css_parser_tests:
            f.write("\n  add_executable(css_parser_tests\n")
            for p in css_parser_tests:
                f.write(f"    {p}\n")
            f.write("  )\n")
            f.write("  target_compile_options(css_parser_tests PRIVATE -fno-exceptions)\n")
            f.write(
                "  target_link_libraries(css_parser_tests PRIVATE donner_css_parser "
                "donner_css_selector_test_utils gtest_main donner_base_test_utils)\n"
            )
            f.write("  add_test(NAME css_parser_tests COMMAND css_parser_tests)\n")
            f.write(
                "  set_tests_properties(css_parser_tests PROPERTIES ENVIRONMENT "
                "\"RUNFILES_DIR=${PROJECT_SOURCE_DIR}\")\n"
            )

        if css_parsing_tests:
            f.write("\n  add_executable(css_parsing_tests\n")
            for p in css_parsing_tests:
                f.write(f"    {p}\n")
            f.write("  )\n")
            f.write("  target_compile_options(css_parsing_tests PRIVATE -fno-exceptions)\n")
            f.write(
                "  target_link_libraries(css_parsing_tests PRIVATE donner_css_parser "
                "gtest_main donner_base_test_utils "
                "nlohmann_json::nlohmann_json)\n"
            )
            f.write("  add_test(NAME css_parsing_tests COMMAND css_parsing_tests)\n")
            f.write(
                "  set_tests_properties(css_parsing_tests PROPERTIES ENVIRONMENT "
                "\"RUNFILES_DIR=${PROJECT_SOURCE_DIR}\")\n"
            )
            f.write("  file(COPY\n")
            for name in [
                "component_value_list.json",
                "declaration_list.json",
                "one_component_value.json",
                "one_declaration.json",
                "one_rule.json",
                "rule_list.json",
                "stylesheet.json",
            ]:
                f.write(f"    ${{PROJECT_SOURCE_DIR}}/third_party/css-parsing-tests/{name}\n")
            f.write("  DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/css-parsing-tests)\n")
        f.write("endif()\n")


def generate_base_support() -> None:
    packages = [
        "donner/base/parser",
        "donner/base/element",
        "donner/base/xml",
    ]
    libs: List[Tuple[str, str]] = []
    for p in packages:
        libs.extend([(p, name) for name in query_cc_libraries(p)])

    path = Path("donner/base") / "CMakeLists.txt"
    with path.open("a") as f:
        f.write("\n# Additional libraries\n")
        for pkg, lib in libs:
            srcs = query_labels("srcs", f"//{pkg}:{lib}", relative_to="donner/base")
            hdrs = query_labels("hdrs", f"//{pkg}:{lib}", relative_to="donner/base")
            target = cmake_target_name(pkg, lib)
            write_library(f, target, srcs, hdrs)
            if target == "donner_base_parser":
                f.write("target_link_libraries(donner_base_parser PUBLIC donner_base)\n")
            elif target == "donner_base_parser_line_offsets":
                f.write("target_link_libraries(donner_base_parser_line_offsets INTERFACE donner_base)\n")
            elif target == "donner_base_element_fake":
                f.write("target_link_libraries(donner_base_element_fake INTERFACE gtest)\n")
            elif target == "donner_base_xml":
                f.write("target_link_libraries(donner_base_xml PUBLIC donner_base)\n")
            elif target == "donner_base_xml_qualified_name":
                f.write(
                    "target_link_libraries(donner_base_xml_qualified_name INTERFACE donner_base)\n"
                )


def main() -> None:
    generate_root()
    generate_base()
    generate_base_support()
    generate_css()


if __name__ == "__main__":
    main()
