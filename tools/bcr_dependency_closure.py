"""Check the configured downstream BCR renderer dependency closure."""

from __future__ import annotations

import argparse
import ast
from pathlib import Path
import re


MAX_MODULE_BYTES = 1024 * 1024
MAX_LABEL_BYTES = 10 * 1024 * 1024
LABEL = re.compile(r"(?:@{1,2}[^/]+)?//[^\s]+\Z")
RUST_NAME = re.compile(r"(?:^|[+~_./:-])(?:rust|cargo|crate)")
REQUIRED_TARGETS = frozenset({
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


def configured_labels(contents: str) -> set[str]:
    labels: set[str] = set()
    for raw in contents.splitlines():
        if not raw:
            continue
        label = raw.split(" (", 1)[0]
        if not LABEL.fullmatch(label):
            raise ValueError("configured dependency query contains a non-label line")
        labels.add(label)
    if not labels:
        raise ValueError("configured dependency query returned no labels")
    return labels


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


def check_external_repositories(labels: set[str], dev_repositories: set[str]) -> None:
    if any(repository_parts(label) & dev_repositories for label in labels):
        raise ValueError("renderer closure includes development-only repositories")
    if any(
        RUST_NAME.search(part) or "webgpu" in part or "wgpu" in part
        for label in labels for part in repository_parts(label)
    ):
        raise ValueError("renderer closure includes Rust or WebGPU repositories")


def check_closure(labels: set[str], dev_repositories: set[str]) -> None:
    check_backend(labels)
    check_external_repositories(labels, dev_repositories)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module", required=True, type=Path)
    parser.add_argument("--labels", required=True, type=Path)
    args = parser.parse_args()
    if args.module.stat().st_size > MAX_MODULE_BYTES or args.labels.stat().st_size > MAX_LABEL_BYTES:
        raise ValueError("module or configured closure exceeds its size limit")
    names = dev_repository_names(args.module.read_text(encoding="utf-8"))
    labels = configured_labels(args.labels.read_text(encoding="utf-8"))
    check_closure(labels, names)
    print(f"BCR renderer closure: {len(labels)} labels; tiny-skia and base text; no dev, Geode, WebGPU, or Rust dependencies")


if __name__ == "__main__":
    try:
        main()
    except (OSError, SyntaxError, ValueError) as error:
        raise SystemExit(f"BCR renderer dependency check failed: {error}") from None
