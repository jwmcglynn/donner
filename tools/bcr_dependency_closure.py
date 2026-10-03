"""Check the configured downstream BCR dependency closure of Donner's consumer libraries.

The input is `bazel cquery --output=label_kind` over the libraries a downstream consumer builds.
The closure must contain the default tiny-skia renderer and basic text, and no development-only,
Geode, WebGPU or test target. It must contain no Rust rule, and no repository of a Rust rule set,
toolchain or crate. After the real closure passes, the check also proves that it rejects Rust edges
injected into that same closure, so a vacuous pass fails.
"""

from __future__ import annotations

import argparse
import ast
from pathlib import Path
import re


MAX_MODULE_BYTES = 1024 * 1024
MAX_LABEL_BYTES = 10 * 1024 * 1024
LABEL = re.compile(r"(?:@{1,2}[^/]+)?//[^\s]+\Z")
# One `cquery --output=label_kind` line: the target kind, the label, then its configuration.
KINDED_TARGET = re.compile(
    r"(?P<kind>[A-Za-z_][\w]*(?: [a-z]+)?) (?P<label>(?:@{1,2}[^/\s]+)?//\S+)(?: \([^)]*\))?\Z"
)
RUST_NAME = re.compile(r"(?:^|[+~_./:-])(?:rust|cargo|crate)")
RUST_RULE = re.compile(r"(?:^|_)(?:rust|cargo|crate)")
REQUIRED_TARGETS = frozenset({
    "//donner/base:base",
    "//donner/css:css",
    "//donner/svg:svg",
    "//donner/svg/renderer:renderer",
    "//donner/svg/renderer:renderer_tiny_skia",
    "//donner/svg/renderer:RendererTinySkiaBackend.cc",
    "//donner/svg/text:text_backend_simple",
    "//donner/svg/text:TextBackendSimple.cc",
})
FORBIDDEN_TARGETS = frozenset({
    "//donner/svg/renderer:renderer_geode",
    "//donner/svg/renderer:RendererGeodeBackend.cc",
    "//donner/svg/renderer:RendererGeode.cc",
    "//donner/svg/text:text_backend_full",
    "//donner/svg/text:TextBackendFull.cc",
})
FORBIDDEN_PACKAGES = (
    "//donner/gpu",
    "//donner/svg/renderer/geode",
    "//third_party/webgpu-cpp",
)
# Rust edges the check must reject once injected into a passing closure: an in-tree Rust rule, and
# the rule set, toolchain and crate repositories under Bazel 8 (`+`) and Bazel 7 (`~`) names.
INJECTED_RUST_EDGES = (
    ("rust_library rule", "@@donner+//donner/svg:injected_edge"),
    ("toolchain rule", "@@rules_rust+//rust/private:injected_edge"),
    ("toolchain rule", "@@rules_rust~//rust/private:injected_edge"),
    ("alias rule", "@@rules_rust++rust+rust_toolchains//:injected_edge"),
    ("alias rule", "@@rules_rust~~rust~rust_toolchains//:injected_edge"),
    ("filegroup rule", "@@rules_rust++crate+crates__injected-1.0.0//:injected_edge"),
    ("filegroup rule", "@@rules_rust~~crate~crates__injected-1.0.0//:injected_edge"),
)


def keyword(call: ast.Call, name: str) -> object | None:
    for item in call.keywords:
        if item.arg == name:
            return ast.literal_eval(item.value)
    return None


def dev_extensions(tree: ast.Module) -> set[str]:
    extensions: set[str] = set()
    for statement in tree.body:
        if not isinstance(statement, ast.Assign) or not isinstance(statement.value, ast.Call):
            continue
        call = statement.value
        if isinstance(call.func, ast.Name) and call.func.id == "use_extension":
            if keyword(call, "dev_dependency") is True:
                extensions.update(target.id for target in statement.targets
                                  if isinstance(target, ast.Name))
    return extensions


def direct_dev_names(node: ast.AST) -> set[str]:
    if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Name):
        return set()
    if node.func.id not in {"bazel_dep", "http_file", "new_local_repository", "local_repository"}:
        return set()
    if keyword(node, "dev_dependency") is not True:
        return set()
    return {value for field in ("name", "repo_name")
            if isinstance((value := keyword(node, field)), str)}


def declared_dev_repositories(tree: ast.Module) -> set[str]:
    names: set[str] = set()
    for node in ast.walk(tree):
        names.update(direct_dev_names(node))
    return names


def use_repo_names(call: ast.Call, extensions: set[str]) -> set[str]:
    if not isinstance(call.func, ast.Name) or call.func.id != "use_repo":
        return set()
    if not call.args or not isinstance(call.args[0], ast.Name):
        return set()
    if call.args[0].id not in extensions:
        return set()
    names = {ast.literal_eval(arg) for arg in call.args[1:]}
    names.update(item.arg for item in call.keywords if item.arg)
    names.update(ast.literal_eval(item.value) for item in call.keywords)
    return names


def extension_rule_name(call: ast.Call, extensions: set[str]) -> set[str]:
    if not isinstance(call.func, ast.Attribute) or not isinstance(call.func.value, ast.Name):
        return set()
    if call.func.value.id not in extensions:
        return set()
    value = keyword(call, "name")
    return {value} if isinstance(value, str) else set()


def extension_dev_repositories(tree: ast.Module, extensions: set[str]) -> set[str]:
    names: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            names.update(use_repo_names(node, extensions))
            names.update(extension_rule_name(node, extensions))
    return names


def dev_repository_names(module_text: str) -> set[str]:
    tree = ast.parse(module_text)
    names = declared_dev_repositories(tree)
    names.update(extension_dev_repositories(tree, dev_extensions(tree)))
    if not names:
        raise ValueError("MODULE.bazel declared no development-only repositories")
    return names


def configured_targets(contents: str) -> dict[str, str]:
    """Map each label of a `cquery --output=label_kind` result to its kind."""
    targets: dict[str, str] = {}
    for raw in contents.splitlines():
        if not raw:
            continue
        match = KINDED_TARGET.fullmatch(raw)
        if not match or not LABEL.fullmatch(match.group("label")):
            raise ValueError("configured dependency query contains a line without a kind and label")
        targets[match.group("label")] = match.group("kind")
    if not targets:
        raise ValueError("configured dependency query returned no labels")
    return targets


def repository_parts(label: str) -> set[str]:
    if not label.startswith("@"):
        return set()
    repository = label.lstrip("@").split("//", 1)[0]
    return set(re.split(r"[+~]", repository)) - {""}


def local_target(label: str) -> str:
    return "//" + label.split("//", 1)[1]


def forbidden_package(target: str) -> bool:
    return any(target.startswith(package + separator)
               for package in FORBIDDEN_PACKAGES for separator in (":", "/"))


def test_target(target: str) -> bool:
    package, _, name = target[2:].partition(":")
    return (any(part in {"tests", "testdata"} for part in package.split("/"))
            or name.endswith(("_test", "_tests")) or name.startswith("test_"))


def check_backend(labels: set[str]) -> None:
    donner_labels = {label for label in labels if "donner" in repository_parts(label)}
    targets = {local_target(label) for label in donner_labels}
    if not REQUIRED_TARGETS.issubset(targets):
        raise ValueError("default renderer is missing tiny-skia or base-text targets")
    if not any("tiny-skia-cpp" in repository_parts(label)
               and local_target(label) == "//src:tiny_skia_lib" for label in labels):
        raise ValueError("default renderer has no tiny-skia library")
    if targets & FORBIDDEN_TARGETS or any(
        forbidden_package(target) or test_target(target) for target in targets
    ):
        raise ValueError("default renderer includes Geode, WebGPU, full-text, or test targets")
    if any(RUST_NAME.search(target) for target in targets):
        raise ValueError("default renderer includes an in-tree Rust target")


def check_rules(targets: dict[str, str]) -> None:
    for kind in targets.values():
        if kind.endswith(" rule") and RUST_RULE.search(kind.removesuffix(" rule")):
            raise ValueError("closure includes a Rust rule")


def check_external_repositories(labels: set[str], dev_repositories: set[str]) -> None:
    # Rust is reported first, so a Rust repository that is also development-only names Rust.
    if any(
        RUST_NAME.search(part) or "webgpu" in part or "wgpu" in part
        for label in labels for part in repository_parts(label)
    ):
        raise ValueError("closure includes Rust or WebGPU repositories")
    if any(repository_parts(label) & dev_repositories for label in labels):
        raise ValueError("closure includes development-only repositories")


def check_closure(targets: dict[str, str], dev_repositories: set[str]) -> None:
    labels = set(targets)
    check_backend(labels)
    check_rules(targets)
    check_external_repositories(labels, dev_repositories)


def check_injected_rust_edges(targets: dict[str, str], dev_repositories: set[str]) -> None:
    """Prove that the check rejects each Rust edge added to this passing closure."""
    for kind, label in INJECTED_RUST_EDGES:
        try:
            check_closure({**targets, label: kind}, dev_repositories)
        except ValueError as error:
            if "Rust" not in str(error):
                raise ValueError(f"injected {kind} {label} failed for another reason: {error}")
            continue
        raise ValueError(f"the check accepted an injected Rust edge: {kind} {label}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module", required=True, type=Path)
    parser.add_argument("--labels", required=True, type=Path)
    args = parser.parse_args()
    if args.module.stat().st_size > MAX_MODULE_BYTES or args.labels.stat().st_size > MAX_LABEL_BYTES:
        raise ValueError("module or configured closure exceeds its size limit")
    names = dev_repository_names(args.module.read_text(encoding="utf-8"))
    targets = configured_targets(args.labels.read_text(encoding="utf-8"))
    check_closure(targets, names)
    check_injected_rust_edges(targets, names)
    print(f"BCR consumer closure: {len(targets)} labels; tiny-skia and base text; no dev, Geode, "
          "WebGPU or test targets; no Rust rule, toolchain or crate; "
          f"{len(INJECTED_RUST_EDGES)} injected Rust edges rejected")


if __name__ == "__main__":
    try:
        main()
    except (OSError, SyntaxError, ValueError) as error:
        raise SystemExit(f"BCR consumer dependency check failed: {error}") from None
