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
import ast
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import (
    Any,
    DefaultDict,
    Dict,
    FrozenSet,
    Iterable,
    List,
    Optional,
    Set,
    Tuple,
)

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

_EXTERNAL_DEPS_MANIFEST = Path(__file__).with_name("external_deps.json")


@dataclass(frozen=True)
class CMakeConfig:
    """One Bazel configuration sampled by the CMake generator."""

    name: str
    bazel_args: Tuple[str, ...]
    text: bool
    text_full: bool
    filters: bool


# The matrix covers every valid combination of the public CMake feature
# booleans (`DONNER_TEXT_FULL` implies `DONNER_TEXT`).  Per-config Bazel attrs
# are merged into CMake conditions from this truth table.
CMAKE_CONFIGS: Tuple[CMakeConfig, ...] = (
    CMakeConfig("default", (), text=True, text_full=False, filters=True),
    CMakeConfig("no_text", ("--config=no-text",), text=False, text_full=False, filters=True),
    CMakeConfig("no_filters", ("--config=no-filters",), text=True, text_full=False, filters=False),
    CMakeConfig("tiny", ("--config=tiny",), text=False, text_full=False, filters=False),
    CMakeConfig("text_full", ("--config=text-full",), text=True, text_full=True, filters=True),
    CMakeConfig(
        "text_full_no_filters",
        ("--config=text-full", "--config=no-filters"),
        text=True,
        text_full=True,
        filters=False,
    ),
)

_CONFIG_BY_NAME = {config.name: config for config in CMAKE_CONFIGS}
_ALL_CONFIG_NAMES: FrozenSet[str] = frozenset(_CONFIG_BY_NAME)


@dataclass(frozen=True)
class ExternalDepResolution:
    """How a Bazel external label is represented in generated CMake."""

    kind: str
    value: Optional[str] = None
    include_dirs: Optional[str] = None


@dataclass(frozen=True)
class ExternalDepManifest:
    """Parsed external dependency manifest."""

    targets: Dict[str, str]
    system: Dict[str, Tuple[str, str]]
    ignored: Set[str]
    ignored_prefixes: Tuple[Tuple[str, Tuple[str, ...]], ...]


_external_dep_manifest_cache: Optional[ExternalDepManifest] = None


def _load_external_dep_manifest() -> ExternalDepManifest:
    """Load the declarative Bazel-label → CMake external dependency manifest."""
    global _external_dep_manifest_cache
    if _external_dep_manifest_cache is not None:
        return _external_dep_manifest_cache

    data = json.loads(_EXTERNAL_DEPS_MANIFEST.read_text())
    targets: Dict[str, str] = {}
    system: Dict[str, Tuple[str, str]] = {}
    ignored: Set[str] = set()
    ignored_prefixes: List[Tuple[str, Tuple[str, ...]]] = []

    for entry in data.get("targets", []):
        cmake_target = entry["cmake"]
        for label in entry.get("labels", []):
            targets[label] = cmake_target

    for entry in data.get("system", []):
        libraries = entry["libraries"]
        include_dirs = entry["include_dirs"]
        for label in entry.get("labels", []):
            system[label] = (libraries, include_dirs)

    for entry in data.get("ignore", []):
        ignored.update(entry.get("labels", []))

    for entry in data.get("ignore_prefixes", []):
        except_labels = tuple(entry.get("except_labels", []))
        for prefix in entry.get("prefixes", []):
            ignored_prefixes.append((prefix, except_labels))

    _external_dep_manifest_cache = ExternalDepManifest(
        targets=targets,
        system=system,
        ignored=ignored,
        ignored_prefixes=tuple(ignored_prefixes),
    )
    return _external_dep_manifest_cache


def _resolve_external_dep(dep: str) -> Optional[ExternalDepResolution]:
    """Resolve an external Bazel dep using tools/cmake/external_deps.json."""
    manifest = _load_external_dep_manifest()
    if dep in manifest.targets:
        return ExternalDepResolution("target", manifest.targets[dep])
    if dep in manifest.system:
        libraries, include_dirs = manifest.system[dep]
        return ExternalDepResolution("system", libraries, include_dirs)
    if dep in manifest.ignored:
        return ExternalDepResolution("ignore")

    for prefix, except_labels in manifest.ignored_prefixes:
        if dep.startswith(prefix) and dep not in except_labels:
            return ExternalDepResolution("ignore")

    return None

# Helper constants for CMake condition strings.
_TINY_SKIA = 'DONNER_RENDERER_BACKEND STREQUAL "tiny_skia"'
_TEXT_FULL = "DONNER_TEXT_FULL"

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


_CMAKE_LEAF_TARGET_PATTERNS = (
    "//:donner",
)
_CMAKE_LEAF_TARGET_EXPRESSION = "(" + " + ".join(_CMAKE_LEAF_TARGET_PATTERNS) + ")"
_CQUERY_TARGET_EXPRESSION = (
    'kind(".*cc_.*|embed_resources_generate_header", '
    f"deps({_CMAKE_LEAF_TARGET_EXPRESSION}))"
)


@dataclass
class ConfiguredTargetInfo:
    """Resolved attrs for one target in one Bazel configuration."""

    label: str
    package: str
    name: str
    kind: str
    attrs: Dict[str, Any]
    compatible: bool


@dataclass
class CMakeTarget:
    """Merged CMake target data across the sampled Bazel configurations."""

    label: str
    package: str
    name: str
    kind: str
    configs: Set[str]
    values: Dict[str, Dict[str, Set[str]]]


_cmake_targets_cache: Optional[Dict[str, CMakeTarget]] = None


def _normalize_rule_kind(rule_class: str) -> Optional[str]:
    """Map a Bazel rule class from cquery output to the generator's target kind."""
    if rule_class.endswith("embed_resources_generate_header"):
        return "embed_resources"
    if rule_class.endswith("cc_library"):
        return "cc_library"
    if rule_class.endswith("cc_binary"):
        return "cc_binary"
    if rule_class.endswith("cc_test"):
        return "cc_test"
    return None


def _split_build_attrs(body: str) -> Dict[str, Any]:
    """Parse the small Python-like attr syntax from `bazel cquery --output=build`."""
    attrs: Dict[str, Any] = {}
    current_key: Optional[str] = None
    current_value: List[str] = []
    bracket_depth = 0

    def finish_attr() -> None:
        nonlocal current_key, current_value, bracket_depth
        if current_key is None:
            return
        raw_value = "\n".join(current_value).strip()
        if raw_value.endswith(","):
            raw_value = raw_value[:-1].rstrip()
        try:
            attrs[current_key] = ast.literal_eval(raw_value)
        except (SyntaxError, ValueError):
            attrs[current_key] = raw_value.strip('"')
        current_key = None
        current_value = []
        bracket_depth = 0

    for line in body.splitlines():
        stripped = line.strip()
        if not stripped:
            continue

        if current_key is None:
            if " = " not in stripped:
                continue
            key, value = stripped.split(" = ", 1)
            current_key = key
            current_value = [value]
        else:
            current_value.append(stripped)

        bracket_depth += stripped.count("[") - stripped.count("]")
        if bracket_depth <= 0 and stripped.endswith(","):
            finish_attr()

    finish_attr()
    return attrs


def _parse_build_rules(path: Path, allowed_rule_names: Set[str]) -> Dict[str, Dict[str, Any]]:
    """Parse simple Bazel rule calls from a BUILD file."""
    text = path.read_text()
    pattern = re.compile(
        r"^([A-Za-z0-9_]+)\(\n(.*?)^\)\n",
        re.MULTILINE | re.DOTALL,
    )
    rules: Dict[str, Dict[str, Any]] = {}
    for match in pattern.finditer(text):
        rule_name = match.group(1)
        if rule_name not in allowed_rule_names:
            continue

        attrs = _split_build_attrs(match.group(2))
        name = str(attrs.get("name", ""))
        if not name:
            continue
        attrs["__rule_name"] = rule_name
        rules[name] = attrs

    return rules


def _package_from_build_comment(comment: str, workspace: Path) -> Optional[str]:
    """Extract a Bazel package path from a cquery `# /path/BUILD.bazel` comment."""
    location = comment.removeprefix("# ").split(":", 1)[0]
    path = Path(location)
    try:
        rel = path.relative_to(workspace)
    except ValueError:
        return None
    if rel.name != "BUILD.bazel":
        return None
    return "" if rel.parent == Path(".") else rel.parent.as_posix()


def _is_compatible(attrs: Dict[str, Any]) -> bool:
    target_compatible_with = attrs.get("target_compatible_with", [])
    if not isinstance(target_compatible_with, list):
        return True
    return "@platforms//:incompatible" not in target_compatible_with


def _parse_cquery_build_output(output: str, workspace: Path) -> List[ConfiguredTargetInfo]:
    """Parse configured target attrs from `bazel cquery --output=build`."""
    pattern = re.compile(
        r"^(# /[^\n]+/BUILD\.bazel:\d+:\d+)\n([A-Za-z0-9_]+)\(\n(.*?)^\)\n",
        re.MULTILINE | re.DOTALL,
    )
    result: List[ConfiguredTargetInfo] = []
    for match in pattern.finditer(output):
        package = _package_from_build_comment(match.group(1), workspace)
        if package is None:
            continue

        kind = _normalize_rule_kind(match.group(2))
        if kind is None:
            continue

        attrs = _split_build_attrs(match.group(3))
        name = str(attrs.get("name", ""))
        if not name:
            continue

        if kind == "embed_resources":
            name = str(attrs.get("generator_name", name.removesuffix("_header_gen")))

        label = f"//{package}:{name}" if package else f"//:{name}"
        result.append(
            ConfiguredTargetInfo(
                label=label,
                package=package,
                name=name,
                kind=kind,
                attrs=attrs,
                compatible=_is_compatible(attrs),
            )
        )

    return result


def _query_configured_targets(config: CMakeConfig) -> List[ConfiguredTargetInfo]:
    """Return configured target attrs for one CMake feature config."""
    args = [
        "cquery",
        _CQUERY_TARGET_EXPRESSION,
        "--output=build",
        "--//build_defs:disable_backend_test_transition=true",
        "--//build_defs:disable_perf_opt_transition=true",
    ]
    args.extend(config.bazel_args)
    output = _run_bazel(args)
    targets = _parse_cquery_build_output(output, Path.cwd())
    if not targets:
        raise RuntimeError(f"No C++ targets found for CMake config {config.name}")
    return targets


def _target_value_map() -> Dict[str, Dict[str, Set[str]]]:
    return {
        "srcs": {},
        "hdrs": {},
        "deps": {},
        "copts": {},
        "defines": {},
        "includes": {},
        "tags": {},
    }


def _normalize_dep_label(label: str) -> str:
    if label.startswith("@donner//"):
        return "//" + label.split("//", 1)[1]
    return label


def _label_to_pkg_relative_path(label: str, pkg: str) -> Optional[str]:
    prefix = f"//{pkg}:"
    if label.startswith(prefix):
        rel = label[len(prefix):]
        return str(Path(rel))
    return None


def _list_attr(attrs: Dict[str, Any], name: str) -> List[str]:
    value = attrs.get(name, [])
    if isinstance(value, list):
        return [str(item) for item in value]
    return []


def _record_values(
    target: CMakeTarget,
    attr: str,
    values: Iterable[str],
    config_name: str,
) -> None:
    dest = target.values[attr]
    for value in values:
        dest.setdefault(value, set()).add(config_name)


_TINY_SKIA_ROOT = Path("third_party/tiny-skia-cpp")
_TINY_SKIA_ENTRY_LABELS = (
    "//src:tiny_skia_lib",
    "//src/tiny_skia/filter:filter",
)
_TINY_SKIA_BUILD_RULES = {
    "cc_library",
    "filegroup",
    "tiny_skia_cc_library",
}


def _tiny_skia_build_file(package: str) -> Path:
    return _TINY_SKIA_ROOT / package / "BUILD.bazel"


def _split_tiny_skia_label(label: str, current_package: str) -> Optional[Tuple[str, str]]:
    if label.startswith("@"):
        return None
    if label.startswith(":"):
        return current_package, label.removeprefix(":")
    if label.startswith("//"):
        rest = label.removeprefix("//")
        if ":" not in rest:
            return rest, Path(rest).name
        package, name = rest.split(":", 1)
        return package, name
    return None


def _load_tiny_skia_rules(package: str) -> Dict[str, Dict[str, Any]]:
    return _parse_build_rules(_tiny_skia_build_file(package), _TINY_SKIA_BUILD_RULES)


def _collect_tiny_skia_sources_for_label(
    label: str,
    *,
    current_package: str = "",
    visited_targets: Optional[Set[Tuple[str, str]]] = None,
    seen_sources: Optional[Set[str]] = None,
) -> List[str]:
    """Collect transitive tiny-skia C++ sources from Bazel rules."""
    if visited_targets is None:
        visited_targets = set()
    if seen_sources is None:
        seen_sources = set()

    split = _split_tiny_skia_label(label, current_package)
    if split is None:
        return []

    package, name = split
    target_key = (package, name)
    if target_key in visited_targets:
        return []
    visited_targets.add(target_key)

    rules = _load_tiny_skia_rules(package)
    attrs = rules.get(name)
    if attrs is None:
        raise RuntimeError(f"tiny-skia Bazel target not found: //{package}:{name}")

    sources: List[str] = []
    for src in _list_attr(attrs, "srcs"):
        src_label = _split_tiny_skia_label(src, package)
        if src_label is not None:
            for resolved in _collect_tiny_skia_sources_for_label(
                src,
                current_package=package,
                visited_targets=visited_targets,
                seen_sources=seen_sources,
            ):
                sources.append(resolved)
            continue

        if not src.endswith((".cc", ".cpp", ".cxx")):
            continue
        source_path = (Path(package) / src).as_posix()
        if source_path not in seen_sources:
            seen_sources.add(source_path)
            sources.append(source_path)

    for dep in _list_attr(attrs, "deps"):
        for resolved in _collect_tiny_skia_sources_for_label(
            dep,
            current_package=package,
            visited_targets=visited_targets,
            seen_sources=seen_sources,
        ):
            sources.append(resolved)

    return sources


def _collect_tiny_skia_sources() -> List[str]:
    sources: List[str] = []
    visited_targets: Set[Tuple[str, str]] = set()
    seen_sources: Set[str] = set()
    for label in _TINY_SKIA_ENTRY_LABELS:
        sources.extend(
            _collect_tiny_skia_sources_for_label(
                label,
                visited_targets=visited_targets,
                seen_sources=seen_sources,
            )
        )
    return sources


_MANUALLY_HANDLED_CMAKE_PACKAGES = {
    "",
    "pixelmatch-cpp17",
    "third_party",
    "third_party/stb",
    "third_party/tiny-skia-cpp",
}


def _is_skipped_package(pkg: str) -> bool:
    return pkg in _MANUALLY_HANDLED_CMAKE_PACKAGES


def _intersect_target_values_with_configs(target: CMakeTarget) -> None:
    """Drop per-attr config entries that are outside the target's configs."""
    for value_map in target.values.values():
        for value, configs in list(value_map.items()):
            configs &= target.configs
            if not configs:
                value_map.pop(value)


def _constrain_target_configs_by_dependency_compatibility(
    targets: Dict[str, CMakeTarget],
) -> None:
    """Constrain targets that directly require deps unavailable in some configs.

    Bazel `cquery --output=build` can report an unconditional dep edge even when
    the referenced target is incompatible in part of the sampled config matrix.
    Such a target cannot be emitted in CMake for those configs unless its source
    files are also split by config. Constrain it to the configs where all direct
    internal deps exist, then propagate that restriction through dependents.
    """
    changed = True
    while changed:
        changed = False
        for label, target in targets.items():
            if label == "//:donner" or target.kind == "embed_resources" or not target.configs:
                continue

            allowed_configs = set(target.configs)
            for dep, dep_configs in target.values["deps"].items():
                dep_target = targets.get(dep)
                if dep_target is None:
                    continue

                relevant_dep_configs = set(dep_configs) & target.configs
                missing_configs = relevant_dep_configs - dep_target.configs
                if missing_configs:
                    allowed_configs -= missing_configs

            if allowed_configs != target.configs:
                print(
                    f"Constraining target {label}: direct deps are unavailable in "
                    f"{sorted(target.configs - allowed_configs)}"
                )
                target.configs = allowed_configs
                _intersect_target_values_with_configs(target)
                changed = True


def _aggregate_configured_targets() -> Dict[str, CMakeTarget]:
    """Merge configured Bazel attrs into CMake target metadata."""
    targets: Dict[str, CMakeTarget] = {}

    for config in CMAKE_CONFIGS:
        print(f"Querying configured Bazel graph for CMake config: {config.name}")
        for info in _query_configured_targets(config):
            if info.label != "//:donner" and _is_skipped_package(info.package):
                continue
            if not info.compatible:
                continue

            existing = targets.get(info.label)
            if existing is not None and existing.kind == "embed_resources" and info.kind != "embed_resources":
                continue
            if existing is not None and info.kind == "embed_resources" and existing.kind != "embed_resources":
                existing = None

            if existing is None:
                existing = CMakeTarget(
                    label=info.label,
                    package=info.package,
                    name=info.name,
                    kind=info.kind,
                    configs=set(),
                    values=_target_value_map(),
                )
                targets[info.label] = existing

            existing.configs.add(config.name)
            if info.kind == "embed_resources":
                continue

            srcs = [
                rel
                for label in _list_attr(info.attrs, "srcs")
                if (rel := _label_to_pkg_relative_path(label, info.package)) is not None
            ]
            hdrs = [
                rel
                for label in _list_attr(info.attrs, "hdrs")
                if (rel := _label_to_pkg_relative_path(label, info.package)) is not None
            ]
            copts = [
                opt
                for opt in _list_attr(info.attrs, "copts")
                if not opt.startswith("-I") and not opt.startswith("-isystem")
            ]

            _record_values(existing, "srcs", srcs, config.name)
            _record_values(existing, "hdrs", hdrs, config.name)
            _record_values(existing, "deps", map(_normalize_dep_label, _list_attr(info.attrs, "deps")), config.name)
            _record_values(existing, "copts", copts, config.name)
            _record_values(existing, "defines", _list_attr(info.attrs, "defines"), config.name)
            _record_values(existing, "includes", _list_attr(info.attrs, "includes"), config.name)
            _record_values(existing, "tags", _list_attr(info.attrs, "tags"), config.name)

    # Drop targets that only exist to reach unsupported CMake packages.
    changed = True
    while changed:
        changed = False
        for label, target in list(targets.items()):
            if label == "//:donner" or target.kind == "embed_resources":
                continue
            unsupported = False
            for dep in target.values["deps"]:
                if dep.startswith("//"):
                    dep_pkg = dep.removeprefix("//").split(":", 1)[0]
                    if _is_skipped_package(dep_pkg) or dep not in targets:
                        if _resolve_external_dep(dep) is None:
                            unsupported = True
                            break
            if unsupported:
                print(f"Skipping target {label}: depends on unsupported CMake package")
                targets.pop(label)
                changed = True

    _constrain_target_configs_by_dependency_compatibility(targets)

    for label, target in list(targets.items()):
        if label != "//:donner" and target.kind != "embed_resources" and not target.configs:
            print(f"Skipping target {label}: no supported CMake configs remain")
            targets.pop(label)

    changed = True
    while changed:
        changed = False
        for label, target in list(targets.items()):
            if label == "//:donner" or target.kind == "embed_resources":
                continue
            unsupported = False
            for dep in target.values["deps"]:
                if dep.startswith("//"):
                    dep_pkg = dep.removeprefix("//").split(":", 1)[0]
                    if _is_skipped_package(dep_pkg) or dep not in targets:
                        if _resolve_external_dep(dep) is None:
                            unsupported = True
                            break
            if unsupported:
                print(f"Skipping target {label}: depends on unsupported CMake package")
                targets.pop(label)
                changed = True

    return targets


def get_cmake_targets() -> Dict[str, CMakeTarget]:
    """Return all CMake-supported Bazel C++ targets, merged across configs."""
    global _cmake_targets_cache
    if _cmake_targets_cache is None:
        _cmake_targets_cache = _aggregate_configured_targets()
    return _cmake_targets_cache


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


_CONDITION_VARIABLES: Tuple[Tuple[str, str], ...] = (
    ("DONNER_TEXT", "text"),
    ("DONNER_TEXT_FULL", "text_full"),
    ("DONNER_FILTERS", "filters"),
)


def _config_matches_term(config: CMakeConfig, term: Tuple[Optional[bool], ...]) -> bool:
    for index, (_, attr_name) in enumerate(_CONDITION_VARIABLES):
        expected = term[index]
        if expected is None:
            continue
        if getattr(config, attr_name) != expected:
            return False
    return True


def _term_to_condition(term: Tuple[Optional[bool], ...]) -> str:
    parts: List[str] = []
    for index, (cmake_name, _) in enumerate(_CONDITION_VARIABLES):
        expected = term[index]
        if expected is None:
            continue
        parts.append(cmake_name if expected else f"NOT {cmake_name}")
    if not parts:
        return "TRUE"
    if len(parts) == 1:
        return parts[0]
    return " AND ".join(f"({part})" if part.startswith("NOT ") else part for part in parts)


def _condition_for_config_names(config_names: Iterable[str]) -> Optional[str]:
    """Return a compact CMake condition for a set of sampled config names.

    ``None`` means the item is present in every supported config and does not
    need a guard. ``FALSE`` means the item should never be emitted.
    """
    requested = frozenset(config_names)
    if requested == _ALL_CONFIG_NAMES:
        return None
    if not requested:
        return "FALSE"

    candidates: List[Tuple[Tuple[Optional[bool], ...], FrozenSet[str]]] = []
    for text in (None, False, True):
        for text_full in (None, False, True):
            for filters in (None, False, True):
                term = (text, text_full, filters)
                covered = frozenset(
                    config.name
                    for config in CMAKE_CONFIGS
                    if _config_matches_term(config, term)
                )
                if covered and covered.issubset(requested):
                    candidates.append((term, covered))

    candidates.sort(key=lambda item: (sum(value is not None for value in item[0]), _term_to_condition(item[0])))

    best: Optional[Tuple[int, int, int, Tuple[str, ...]]] = None

    def search(index: int, covered: FrozenSet[str], terms: Tuple[Tuple[Optional[bool], ...], ...]) -> None:
        nonlocal best
        if covered == requested:
            rendered = tuple(_term_to_condition(term) for term in terms)
            score = (
                len(terms),
                sum(sum(value is not None for value in term) for term in terms),
                len(" OR ".join(rendered)),
            )
            if best is None or score < best[:3]:
                best = (*score, rendered)
            return
        if index >= len(candidates):
            return
        if best is not None and len(terms) >= best[0]:
            return

        for next_index in range(index, len(candidates)):
            term, term_configs = candidates[next_index]
            if term_configs.issubset(covered):
                continue
            search(next_index + 1, frozenset(covered | term_configs), terms + (term,))

    search(0, frozenset(), tuple())
    if best is None:
        return " OR ".join(
            f"({_term_to_condition((config.text, config.text_full, config.filters))})"
            for config in CMAKE_CONFIGS
            if config.name in requested
        )

    rendered_terms = best[3]
    if len(rendered_terms) == 1:
        return rendered_terms[0]
    return " OR ".join(f"({term})" for term in rendered_terms)


_WOFF2_CMAKE_TARGETS = {
    "donner_base_fonts_woff2_parser",
    "donner_base_fonts_woff2_parser_tests",
    "woff2dec",
}


def _condition_for_target(target: CMakeTarget) -> Optional[str]:
    """Return the CMake condition guarding a target declaration."""
    cmake_name = cmake_target_name(target.package, target.name)
    if cmake_name in _WOFF2_CMAKE_TARGETS:
        return "DONNER_TEXT_WOFF2"
    return _condition_for_config_names(target.configs)


def _condition_for_item(
    target: CMakeTarget,
    item: str,
    item_configs: Set[str],
) -> Optional[str]:
    """Return a CMake guard for a source, define, or dep on ``target``."""
    if item == "DONNER_TEXT_WOFF2_ENABLED" or item in _WOFF2_CMAKE_TARGETS:
        return "DONNER_TEXT_WOFF2"
    if item_configs == target.configs:
        return None
    return _condition_for_config_names(item_configs)


def _targets_by_cmake_name(targets: Dict[str, CMakeTarget]) -> Dict[str, CMakeTarget]:
    """Return CMake target name -> merged Bazel target metadata."""
    return {
        cmake_target_name(target.package, target.name): target
        for label, target in targets.items()
        if label != "//:donner"
    }


def _condition_for_dep(
    target: CMakeTarget,
    dep_name: str,
    dep_configs: Set[str],
    targets_by_cmake_name: Dict[str, CMakeTarget],
) -> Optional[str]:
    """Return the CMake guard for linking ``target`` to ``dep_name``.

    Direct Bazel deps may still appear in configured attrs even when the dep
    target is incompatible in that config. Intersect the edge's configs with
    the referenced target's configs so CMake does not emit an unconditional
    link to a target that is declared behind an option guard.
    """
    effective_configs = set(dep_configs)
    dep_target = targets_by_cmake_name.get(dep_name)
    dep_condition: Optional[str] = None
    if dep_target is not None:
        effective_configs &= dep_target.configs
        if not effective_configs:
            return "FALSE"
        dep_condition = _condition_for_target(dep_target)

    edge_condition = _condition_for_item(target, dep_name, effective_configs)
    if dep_condition is None or edge_condition == dep_condition:
        return edge_condition
    if edge_condition is None:
        return dep_condition
    if edge_condition == "FALSE" or dep_condition == "FALSE":
        return "FALSE"
    return f"({edge_condition}) AND ({dep_condition})"


def _values_for_target(
    target: CMakeTarget,
    attr: str,
) -> Tuple[List[str], List[Tuple[str, str]]]:
    """Split target attr values into unconditional and conditional lists."""
    fixed: List[str] = []
    conditional: List[Tuple[str, str]] = []
    for value, configs in sorted(target.values[attr].items()):
        condition = _condition_for_item(target, value, configs)
        if condition is None:
            fixed.append(value)
        elif condition != "FALSE":
            conditional.append((value, condition))
    return fixed, conditional


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


def generate_tiny_skia_cmake() -> None:
    """Create the generated CMakeLists.txt for the vendored tiny-skia-cpp package."""
    sources = _collect_tiny_skia_sources()
    path = _TINY_SKIA_ROOT / "CMakeLists.txt"
    path.parent.mkdir(parents=True, exist_ok=True)

    with path.open("w") as f:
        f.write("##\n")
        f.write("## Generated by tools/cmake/gen_cmakelists.py - DO NOT EDIT\n")
        f.write("##\n\n")
        f.write("cmake_minimum_required(VERSION 3.20)\n")
        f.write("project(tiny-skia-cpp LANGUAGES CXX)\n\n")

        f.write("set(TINY_SKIA_SOURCES\n")
        for source in sources:
            f.write(f"  {source}\n")
        f.write(")\n\n")

        f.write("function(add_tiny_skia_target TARGET_NAME SIMD_DEFINE)\n")
        f.write("  add_library(${TARGET_NAME} STATIC ${TINY_SKIA_SOURCES})\n")
        f.write("  target_include_directories(${TARGET_NAME} PUBLIC\n")
        f.write("    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>\n")
        f.write("  )\n")
        f.write("  target_compile_features(${TARGET_NAME} PUBLIC cxx_std_20)\n")
        f.write("  target_compile_definitions(${TARGET_NAME} PRIVATE ${SIMD_DEFINE})\n\n")
        f.write("  if(CMAKE_CXX_COMPILER_ID MATCHES \"Clang|GNU\")\n")
        f.write("    target_compile_options(${TARGET_NAME} PRIVATE -Wall -ffp-contract=off)\n")
        f.write("  elseif(MSVC)\n")
        f.write("    target_compile_options(${TARGET_NAME} PRIVATE /W3 /fp:precise)\n")
        f.write("  endif()\n\n")
        f.write("  if(SIMD_DEFINE STREQUAL \"TINYSKIA_CFG_IF_SIMD_NATIVE=1\")\n")
        f.write("    if(CMAKE_CXX_COMPILER_ID MATCHES \"Clang|GNU\")\n")
        f.write("      if(CMAKE_SYSTEM_PROCESSOR MATCHES \"x86_64|AMD64|amd64\")\n")
        f.write("        target_compile_options(${TARGET_NAME} PRIVATE -mavx2 -mfma)\n")
        f.write("      endif()\n")
        f.write("    elseif(MSVC)\n")
        f.write("      if(CMAKE_SYSTEM_PROCESSOR MATCHES \"AMD64\")\n")
        f.write("        target_compile_options(${TARGET_NAME} PRIVATE /arch:AVX2)\n")
        f.write("      endif()\n")
        f.write("    endif()\n")
        f.write("  endif()\n")
        f.write("endfunction()\n\n")
        f.write("add_tiny_skia_target(tiny_skia \"TINYSKIA_CFG_IF_SIMD_NATIVE=1\")\n")
        f.write("add_tiny_skia_target(tiny_skia_scalar \"TINYSKIA_CFG_IF_SIMD_SCALAR=1\")\n")


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
        f.write("    \"Renderer backend: 'tiny_skia' (default)\")\n")
        f.write("option(DONNER_TEXT \"Enable text rendering (stb_truetype)\" ON)\n")
        f.write("option(DONNER_TEXT_FULL \"Enable full text rendering: FreeType + HarfBuzz\" OFF)\n")
        f.write("option(DONNER_TEXT_WOFF2 \"Enable WOFF2 font support (requires DONNER_TEXT)\" ON)\n")
        f.write("option(DONNER_FILTERS \"Enable SVG filter effects\" ON)\n\n")

        # Validation
        f.write("# Validate options\n")
        f.write("if(NOT DONNER_RENDERER_BACKEND STREQUAL \"tiny_skia\")\n")
        f.write("  message(FATAL_ERROR \"DONNER_RENDERER_BACKEND must be 'tiny_skia', got '${DONNER_RENDERER_BACKEND}'\")\n")
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

        # ── stb_truetype (locally vendored) ────────────────────────────
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
            target.package
            for label, target in get_cmake_targets().items()
            if label != "//:donner" and target.kind != "embed_resources"
        } | {
            target.package
            for label, target in get_cmake_targets().items()
            if label != "//:donner" and target.kind == "embed_resources"
        }

        # Skip third‑party packages that are handled manually elsewhere.
        discovered_pkgs = {pkg for pkg in discovered_pkgs if not _is_skipped_package(pkg)}

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

def _scope_for_target(kind: str, has_concrete_sources: bool) -> str:
    if kind in {"cc_binary", "cc_test"}:
        return "PRIVATE"
    return "PUBLIC" if has_concrete_sources else "INTERFACE"


def _compile_option_scope(kind: str, has_concrete_sources: bool) -> str:
    if kind in {"cc_binary", "cc_test"}:
        return "PRIVATE"
    return "PRIVATE" if has_concrete_sources else "INTERFACE"


def _include_scope(kind: str, has_concrete_sources: bool) -> str:
    if kind in {"cc_binary", "cc_test"}:
        return "PRIVATE"
    return "PUBLIC" if has_concrete_sources else "INTERFACE"


def _resolve_cmake_dep(dep: str) -> Optional[ExternalDepResolution]:
    """Map a Bazel dep label to a CMake target/system/ignore resolution."""
    external = _resolve_external_dep(dep)
    if external is not None:
        return external

    if dep.startswith("//"):
        pkg, name = dep.removeprefix("//").split(":", 1)
        return ExternalDepResolution("target", cmake_target_name(pkg, name))

    msg = f"Unmapped external dep: {dep}"
    if msg not in _unmapped_deps:
        _unmapped_deps.append(msg)
        print(f"WARNING: {msg}")
    return None


def _write_guarded_line(f, condition: Optional[str], line: str) -> None:
    if condition is None:
        f.write(line)
    else:
        f.write(f"if({condition})\n")
        f.write(f"  {line}")
        f.write("endif()\n")


def _emit_links_and_system_includes(
    f,
    target: CMakeTarget,
    cmake_name: str,
    scope: str,
    targets_by_cmake_name: Dict[str, CMakeTarget],
) -> None:
    """Emit target_link_libraries and pkg-config include dirs for a target."""
    linked_targets: Dict[Tuple[str, Optional[str]], None] = {}
    system_includes: Dict[Tuple[str, Optional[str]], None] = {}

    for dep, configs in sorted(target.values["deps"].items()):
        resolution = _resolve_cmake_dep(dep)
        if resolution is None or resolution.kind == "ignore":
            continue

        condition = _condition_for_dep(
            target,
            resolution.value or dep,
            configs,
            targets_by_cmake_name,
        )
        if condition == "FALSE":
            continue
        if resolution.kind == "target":
            if resolution.value == cmake_name:
                continue
            linked_targets[(resolution.value or "", condition)] = None
        elif resolution.kind == "system":
            linked_targets[(resolution.value or "", condition)] = None
            if resolution.include_dirs:
                system_includes[(resolution.include_dirs, condition)] = None

    for dep, condition in linked_targets:
        _write_guarded_line(
            f,
            condition,
            f"target_link_libraries({cmake_name} {scope} {dep})\n",
        )

    for include_dirs, condition in system_includes:
        _write_guarded_line(
            f,
            condition,
            f"target_include_directories({cmake_name} PUBLIC {include_dirs})\n",
        )


def generate_all_packages() -> None:
    """Emit a CMakeLists.txt for every internal package discovered with Bazel."""

    print("Discovering configured cc_library, cc_binary, and cc_test targets...")
    targets = get_cmake_targets()
    targets_by_cmake_name = _targets_by_cmake_name(targets)
    by_pkg: DefaultDict[str, List[CMakeTarget]] = DefaultDict(list)
    for label, target in targets.items():
        if label == "//:donner" or _is_skipped_package(target.package):
            continue
        if target.configs:
            by_pkg[target.package].append(target)

    # Per-package generation
    for pkg, entries in by_pkg.items():
        cmake = Path(pkg) / "CMakeLists.txt"
        cmake.parent.mkdir(parents=True, exist_ok=True)
        with cmake.open("w") as f:
            f.write("##\n")
            f.write("## Generated by tools/cmake/gen_cmakelists.py - DO NOT EDIT\n")
            f.write("##\n\n")
            f.write("cmake_minimum_required(VERSION 3.20)\n\n")

            for target in sorted(entries, key=lambda t: (t.kind, t.name)):
                bazel_label = target.label
                kind = target.kind
                tgt = target.name
                cmake_name = cmake_target_name(pkg, tgt)

                if "_fuzzer" in tgt:
                    # Skip fuzzers, they are not built with CMake
                    print(f"Skipping fuzzer {bazel_label}")
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

                hdrs = sorted(target.values["hdrs"])
                srcs, extracted_cond_srcs = _values_for_target(target, "srcs")
                copts, cond_copts = _values_for_target(target, "copts")
                includes, cond_includes = _values_for_target(target, "includes")
                defines, cond_defines = _values_for_target(target, "defines")

                condition = _condition_for_target(target)
                if condition == "FALSE":
                    print(f"Skipping target {bazel_label}: unsupported in CMake configs")
                    continue
                if condition:
                    f.write(f"if({condition})\n")

                print(
                    "Adding target:",
                    cmake_name,
                    f" (kind={kind}, srcs={len(srcs)}, hdrs={len(hdrs)})"
                    + (f" [conditional: {condition}]" if condition else ""),
                )

                # If a target has conditional sources but no fixed sources, it
                # must be created as a concrete (STATIC) library rather than
                # INTERFACE, so that target_sources(PRIVATE ...) works.
                has_concrete_sources = bool(srcs) or bool(extracted_cond_srcs)
                scope = _scope_for_target(kind, has_concrete_sources)
                option_scope = _compile_option_scope(kind, has_concrete_sources)
                include_scope = _include_scope(kind, has_concrete_sources)

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
                            f"target_compile_options({cmake_name} {option_scope} {' '.join(copts)})\n"
                        )
                    for copt, copt_condition in cond_copts:
                        _write_guarded_line(
                            f,
                            copt_condition,
                            f"target_compile_options({cmake_name} {option_scope} {copt})\n",
                        )
                    if includes:
                        for inc in includes:
                            f.write(
                                f"target_include_directories({cmake_name} {include_scope} "
                                f'"${{PROJECT_SOURCE_DIR}}/{pkg}/{inc}")\n'
                            )
                    for inc, inc_condition in cond_includes:
                        _write_guarded_line(
                            f,
                            inc_condition,
                            f"target_include_directories({cmake_name} {include_scope} "
                            f'"${{PROJECT_SOURCE_DIR}}/{pkg}/{inc}")\n',
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
                    for copt, copt_condition in cond_copts:
                        _write_guarded_line(
                            f,
                            copt_condition,
                            f"target_compile_options({cmake_name} {scope} {copt})\n",
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
                    for inc, inc_condition in cond_includes:
                        _write_guarded_line(
                            f,
                            inc_condition,
                            f"target_include_directories({cmake_name} {scope} "
                            f'"${{PROJECT_SOURCE_DIR}}/{pkg}/{inc}")\n',
                        )

                # Emit conditional sources via target_sources()
                for cond_src, cond in extracted_cond_srcs:
                    f.write(f"if({cond})\n")
                    f.write(f"  target_sources({cmake_name} PRIVATE {cond_src})\n")
                    f.write("endif()\n")

                define_scope = scope if kind in {"cc_binary", "cc_test"} else include_scope
                if defines:
                    f.write(
                        f"target_compile_definitions({cmake_name} {define_scope} "
                        f"{' '.join(defines)})\n"
                    )
                for define, define_condition in cond_defines:
                    _write_guarded_line(
                        f,
                        define_condition,
                        f"target_compile_definitions({cmake_name} {define_scope} {define})\n",
                    )

                _emit_links_and_system_includes(
                    f,
                    target,
                    cmake_name,
                    scope,
                    targets_by_cmake_name,
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
        root_target = targets.get("//:donner")
        if root_target is not None:
            for dep, configs in sorted(root_target.values["deps"].items()):
                resolution = _resolve_cmake_dep(dep)
                if resolution is None or resolution.kind != "target" or resolution.value == "donner":
                    continue
                condition = _condition_for_dep(
                    root_target,
                    resolution.value or dep,
                    configs,
                    targets_by_cmake_name,
                )
                if condition == "FALSE":
                    continue
                if condition is None:
                    f.write(f"  target_link_libraries(donner INTERFACE {resolution.value})\n")
                else:
                    f.write(f"  if({condition})\n")
                    f.write(f"    target_link_libraries(donner INTERFACE {resolution.value})\n")
                    f.write("  endif()\n")
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


def _format_command_error(prefix: str, command: List[str], output: str) -> str:
    """Format a subprocess failure for human-readable diagnostics."""
    rendered_output = output.strip() if output.strip() else "(no output captured)"
    return f"{prefix}\n  {' '.join(command)}\n\n{rendered_output}"


def _run_cmake_build_validation(
    source_dir: Path,
    build_dir: Path,
    *,
    jobs: Optional[int] = None,
) -> Optional[str]:
    """Configure and build a generated CMake tree.

    Returns ``None`` on success, or a human-readable error string on failure.
    """
    parallel_jobs = max(1, jobs or os.cpu_count() or 1)

    configure_cmd = ["cmake", "-S", ".", "-B", str(build_dir)]
    try:
        configure_result = subprocess.run(
            configure_cmd,
            cwd=source_dir,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
    except FileNotFoundError:
        return (
            "CMake configure failed:\n"
            f"  {' '.join(configure_cmd)}\n\n"
            "cmake command not found. Install cmake to use --build validation."
        )
    if configure_result.returncode != 0:
        return _format_command_error(
            "CMake configure failed:",
            configure_cmd,
            configure_result.stdout,
        )

    build_cmd = ["cmake", "--build", str(build_dir), "--parallel", str(parallel_jobs)]
    try:
        build_result = subprocess.run(
            build_cmd,
            cwd=source_dir,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
    except FileNotFoundError:
        return (
            "CMake build failed:\n"
            f"  {' '.join(build_cmd)}\n\n"
            "cmake command not found. Install cmake to use --build validation."
        )
    if build_result.returncode != 0:
        return _format_command_error(
            "CMake build failed:",
            build_cmd,
            build_result.stdout,
        )

    return None


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
        "--build",
        action="store_true",
        help=("With --check, also run 'cmake -S . -B <temp build dir>' followed by "
              "'cmake --build' on the generated tree. This is slower than the "
              "default static validation and is intended as an opt-in deep check."),
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Write generated files to this directory instead of the workspace.",
    )
    args = parser.parse_args()
    if args.build and not args.check:
        parser.error("--build requires --check")
    _check_mode = args.check

    if args.check:
        # Generate in-place to the workspace, capturing original contents first.
        # Since CMakeLists.txt files are gitignored, we snapshot them and restore
        # after validation so the workspace is unchanged on exit.
        workspace = Path.cwd()

        # CMakeLists.txt files that are handwritten instead of generated. These
        # are excluded from both cleanup and generated-output validation.
        vendored_prefixes = (
            "bazel-",
            "third_party/stb",
            "third_party/frozen",
            "third_party/css-parsing-tests",
            "examples/cmake_consumer",
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
        build_error: Optional[str] = None
        try:
            generate_root()
            generate_tiny_skia_cmake()
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
            if args.build and not errors and not _unmapped_deps:
                print("Static validation passed; configuring and building generated tree...")
                with tempfile.TemporaryDirectory(prefix="donner-cmake-check-") as temp_dir:
                    build_error = _run_cmake_build_validation(
                        workspace,
                        Path(temp_dir) / "build",
                    )
        finally:
            # Remove anything newly created then restore originals
            for p in list(workspace.rglob("CMakeLists.txt")):
                rel = p.relative_to(workspace)
                if _is_generated_cmake(rel) and rel not in existing_files:
                    p.unlink()
            for rel, content in existing_files.items():
                p = workspace / rel
                p.write_bytes(content)

        had_errors = bool(errors) or bool(_unmapped_deps) or build_error is not None
        if errors:
            print(f"\n{'='*60}")
            print(f"CMakeLists.txt VALIDATION FAILED ({len(errors)} error(s))")
            print(f"{'='*60}")
            for e in errors:
                print(f"  {e}")
        if build_error:
            print(f"\n{'='*60}")
            print("CMakeLists.txt BUILD VALIDATION FAILED")
            print(f"{'='*60}")
            print(build_error)
        if _unmapped_deps:
            print(f"\nUnmapped external dependencies ({len(_unmapped_deps)}):")
            for msg in _unmapped_deps:
                print(f"  {msg}")
            print(
                "\nAdd these to tools/cmake/external_deps.json as a target, "
                "system dependency, or ignored dependency with a reason."
            )

        if had_errors:
            sys.exit(1)
        if args.build:
            print("CMakeLists.txt validation and build passed.")
        else:
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
        generate_tiny_skia_cmake()
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
        print("  -DDONNER_TEXT=OFF                     Disable text rendering")
        print("  -DDONNER_TEXT_FULL=ON                 Enable FreeType + HarfBuzz shaping")
        print("  -DDONNER_TEXT_WOFF2=OFF               Disable WOFF2 support")
        print("  -DDONNER_FILTERS=OFF                  Disable filter effects")


if __name__ == "__main__":
    main()
