#!/usr/bin/env python3
"""Configured no-Rust product closure gate and cross-platform CI receipts.

The root inventory is checked in. Each platform runner asks Bazel for the
configured dependency closure of every root; no source-path grep substitutes
for a selected dependency graph. Receipts bind these queries to a source tree,
inventory, profile, and platform so the final CI job can reject stale/missing
platform work.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
INVENTORY = Path(__file__).with_name("configured_rust_roots.json")
SCHEMA = 1
ARCHIVE_RULE = ROOT / "third_party/bazel/non_bcr_deps.bzl"
LOCK = ROOT / "MODULE.bazel.lock"
ARCHIVE_RE = re.compile(r'\bname\s*=\s*"(wgpu_native_(?:linux|macos)_(?:aarch64|x86_64))"\s*,\s*asset\s*=\s*"[^\"]+"\s*,\s*sha256\s*=\s*"([0-9a-f]{64})"', re.S)
LABEL_RE = re.compile(r"^(?:@@?[^/]+)?//[^\s]+")
BAD_ARTIFACT_RE = re.compile(r"(?:wgpu[_-]?native|webgpu[_-]?cpp|(?:^|/)(?:webgpu|wgpu)\.h(?:pp)?$|\.rlib$|\.rmeta$)", re.I)
INSTALL_RE = re.compile(r"^\s*install\s*\(", re.M | re.I)
REQUIRED_NATIVE_ROOTS = {
    "//:donner", "//donner/svg/tool:donner-svg",
    "//donner/editor:editor_impl", "//donner/editor:editor",
}
REQUIRED_BROWSER_ROOTS = {
    "//donner/editor/wasm:wasm_web_package",
    "//donner/svg/renderer/wasm:donner_wasm_geode",
    "//donner/svg/renderer/wasm:geode_browser_test_package",
}
REQUIRED_GEODE_ROOTS = {
    "//:donner", "//donner/svg/tool:donner-svg",
    "//donner/svg/renderer:renderer_geode", "//donner/svg/renderer/geode:geode_device",
    "//donner/editor:editor_impl", "//examples:geode_embed",
}
REQUIRED_LINUX_ARCHIVES = {"wgpu_native_linux_aarch64", "wgpu_native_linux_x86_64"}


class GateError(RuntimeError):
    pass


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical(data: Any) -> bytes:
    return json.dumps(data, sort_keys=True, separators=(",", ":")).encode()


def run(*args: str, cwd: Path = ROOT) -> str:
    proc = subprocess.run(args, cwd=cwd, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=False)
    if proc.returncode:
        raise GateError(f"{' '.join(args)} failed ({proc.returncode}):\n{proc.stderr[-6000:]}")
    return proc.stdout


def inventory() -> dict[str, Any]:
    data = json.loads(INVENTORY.read_text())
    if data.get("schema") != SCHEMA or set(data.get("platforms", {})) != {"linux", "macos"}:
        raise GateError("configured root inventory has an unknown schema or platform set")
    if set(data.get("allowedArchiveRepositories", [])) != REQUIRED_LINUX_ARCHIVES:
        raise GateError("archive allowlist must contain exactly the two Linux oracle variants")
    required_fragments = {"//third_party/webgpu-cpp:", "//:wgpu_native", "//tests/rust_ffi:"}
    if not required_fragments.issubset(data.get("forbiddenLabelFragments", [])):
        raise GateError("configured root inventory omitted a forbidden Rust-backed dependency")
    for os_name, profiles in data["platforms"].items():
        if set(profiles) != {"native", "nativeGeode", "browser", "oracle"}:
            raise GateError(f"{os_name}: incomplete closure profile inventory")
        if not profiles["native"] or not profiles["browser"] or (os_name == "linux" and not profiles["oracle"]):
            raise GateError(f"{os_name}: missing required product or oracle root")
        if not REQUIRED_NATIVE_ROOTS.issubset(profiles["native"]) or \
           not REQUIRED_GEODE_ROOTS.issubset(profiles["nativeGeode"]) or \
           not REQUIRED_BROWSER_ROOTS.issubset(profiles["browser"]):
            raise GateError(f"{os_name}: required product or shipped browser root missing")
        for roots in profiles.values():
            if len(roots) != len(set(roots)) or any(not root.startswith("//") for root in roots):
                raise GateError(f"{os_name}: duplicate or invalid configured root")
    if data["platforms"]["macos"]["oracle"]:
        raise GateError("the wgpu-native oracle is Linux-only")
    if data["platforms"]["linux"]["oracle"] != [data["oracleLabel"]]:
        raise GateError("the sole Linux oracle root must match the allowlist")
    artifact_roots = data.get("artifactRoots", {})
    if set(artifact_roots) != {"linux", "macos"}:
        raise GateError("Linux and macOS shipped artifact roots must be declared")
    if set(artifact_roots["linux"].get("native", [])) != {"//donner/svg/tool:donner-svg"} or \
       set(artifact_roots["linux"].get("browser", [])) != {
           "//donner/editor/wasm:wasm_web_package",
           "//donner/svg/renderer/wasm:geode_browser_test_package",
       } or set(artifact_roots["macos"].get("native", [])) != {"//donner/svg/tool:donner-svg"}:
        raise GateError("required CLI or shipped browser artifact root missing")
    return data


def source_identity() -> dict[str, str]:
    if run("git", "status", "--porcelain", "--untracked-files=no").strip():
        raise GateError("source tree has tracked edits; receipt cannot bind the checked-out revision")
    return {
        "commit": run("git", "rev-parse", "HEAD").strip(),
        "tree": run("git", "rev-parse", "HEAD^{tree}").strip(),
        "inventorySha256": digest(INVENTORY.read_bytes()),
    }


def archive_pins() -> dict[str, str]:
    pins = dict(ARCHIVE_RE.findall(ARCHIVE_RULE.read_text()))
    expected = set(inventory()["allowedArchiveRepositories"])
    if set(pins) != expected or any(pin == "0" * 64 for pin in pins.values()):
        raise GateError("wgpu-native fetch rule must contain only the checksum-pinned Linux test oracle archives")
    return pins


def verify_lock(pins: dict[str, str]) -> str:
    if not LOCK.is_file():
        raise GateError("generated MODULE.bazel.lock is missing")
    raw = LOCK.read_bytes()
    lock = json.loads(raw)
    # The module extension's generated-repository specification records the
    # actual http_archive attributes. A top-level source pin alone is not proof.
    extensions = lock.get("moduleExtensions", {})
    extension = extensions.get("//third_party:bazel/non_bcr_deps.bzl%non_bcr_deps", {})
    generated = extension.get("general", {}).get("generatedRepoSpecs", {})
    if {name for name in generated if name.startswith("wgpu_native_")} != set(pins):
        raise GateError("generated lock contains missing or unexpected wgpu-native archive rules")
    for name, sha in pins.items():
        attributes = generated.get(name, {}).get("attributes", {})
        if attributes.get("sha256") != sha:
            raise GateError(f"generated lock does not bind {name} to its reviewed SHA-256")
    return digest(raw)


def parse_cquery_labels(output: str, root: str) -> set[str]:
    labels: set[str] = set()
    for line in output.splitlines():
        line = line.strip()
        if not line:
            continue
        match = LABEL_RE.match(line)
        if not match:
            raise GateError(f"{root}: unrecognized configured Bazel label: {line[:160]}")
        labels.add(match.group())
    if root not in labels:
        raise GateError(f"{root}: missing or incompatible configured root")
    if len(labels) < 2:
        raise GateError(f"{root}: configured closure is empty or incompatible")
    return labels


def forbidden(labels: set[str], fragments: list[str]) -> list[str]:
    return sorted(label for label in labels if any(fragment in label for fragment in fragments))


def check_closure(root: str, profile: str, labels: set[str], spec: dict[str, Any]) -> None:
    hits = forbidden(labels, spec["forbiddenLabelFragments"])
    if profile == "oracle":
        has_wrapper = "//third_party/webgpu-cpp:webgpu_cpp" in labels
        has_linux_archive = any("wgpu_native_linux_" in label and "//:wgpu_native" in label
                                for label in labels)
        forbidden_oracle = [label for label in hits if "wgpu_native_macos" in label or "//tests/rust_ffi:" in label]
        if root != spec["oracleLabel"] or not has_wrapper or not has_linux_archive or forbidden_oracle:
            raise GateError(f"{root}: Linux oracle must reach the pinned test-only wrapper/archive")
        return
    if spec["oracleLabel"] in labels or hits:
        raise GateError(f"{root}: production closure reaches test oracle, WebGPU-C++ wrapper, or Rust archive: {hits}")


def query_closure(root: str, profile: str, bazel: str) -> set[str]:
    args = [bazel, "cquery", f"deps({root})", "--output=label", "--noshow_progress"]
    if profile == "browser":
        args.append("--config=editor-wasm")
    elif profile == "nativeGeode":
        args.append("--config=geode")
    return parse_cquery_labels(run(*args), root)


def check_generated_cmake_texts(texts: dict[str, str]) -> str:
    """Record that Donner currently has no install surface, failing if one appears."""
    if "CMakeLists.txt" not in texts or len(texts) < 2:
        raise GateError("generated Donner CMake tree is missing or incomplete")
    violations = [name for name, content in texts.items() if INSTALL_RE.search(content)]
    if violations:
        raise GateError(f"generated Donner CMake install rules need artifact scanning: {violations}")
    return digest(canonical(texts))


def check_cmake_consumer() -> dict[str, str | bool]:
    """Validate emitted CMake, then build the existing public consumer."""
    run(sys.executable, "tools/cmake/gen_cmakelists.py", "--check")
    run(sys.executable, "tools/cmake/gen_cmakelists.py")
    tracked = set(run("git", "ls-files", "--", "*CMakeLists.txt").splitlines())
    generated = {
        str(path.relative_to(ROOT)): path.read_text()
        for path in ROOT.rglob("CMakeLists.txt")
        if str(path.relative_to(ROOT)) not in tracked and not any(part.startswith("bazel-") for part in path.parts)
    }
    generated_sha = check_generated_cmake_texts(generated)
    with tempfile.TemporaryDirectory(prefix="donner-no-rust-cmake-") as temp:
        build = Path(temp) / "build"
        run("cmake", "-S", "examples/cmake_consumer", "-B", str(build), "-G", "Ninja",
            f"-DDONNER_SOURCE_DIR={ROOT}")
        run("cmake", "--build", str(build), "--target", "donner_cmake_consumer", "--parallel", "2")
        run("ctest", "--test-dir", str(build), "--output-on-failure")
    return {"installSurface": "absent", "consumerPassed": True, "generatedCmakeSha256": generated_sha}


def scan_artifact_output(path: Path, root: str) -> list[dict[str, Any]]:
    """Hash real Bazel outputs, including every member of a tree artifact."""
    if not path.exists():
        raise GateError(f"{root}: Bazel output missing: {path}")
    if path.is_symlink():
        raise GateError(f"{root}: Bazel output is a symlink: {path}")
    entries = sorted(path.rglob("*") if path.is_dir() else [path])
    if any(item.is_symlink() for item in entries):
        raise GateError(f"{root}: symlinked Bazel artifact")
    files = [item for item in entries if item.is_file()]
    if not files:
        raise GateError(f"{root}: empty or symlinked Bazel artifact")
    records = []
    for item in files:
        name = str(item.relative_to(path)) if path.is_dir() else item.name
        if BAD_ARTIFACT_RE.search(name):
            raise GateError(f"{root}: shipped artifact includes Rust-backed GPU path: {name}")
        data = item.read_bytes()
        if not data or b"libwgpu_native" in data or b"webgpu_cpp" in data:
            raise GateError(f"{root}: empty or Rust-backed GPU artifact: {name}")
        records.append({"path": name, "bytes": len(data), "sha256": digest(data)})
    return records


def build_and_scan_artifacts(os_name: str, bazel: str, spec: dict[str, Any]) -> list[dict[str, Any]]:
    receipts = []
    for profile, roots in spec["artifactRoots"][os_name].items():
        for root in roots:
            args = [bazel, "build", root, "--noshow_progress"]
            query = [bazel, "cquery", root, "--output=files", "--noshow_progress"]
            if profile == "browser":
                args.append("--config=editor-wasm")
                query.append("--config=editor-wasm")
            run(*args)
            outputs = [line.strip() for line in run(*query).splitlines() if line.strip()]
            if not outputs:
                raise GateError(f"{root}: Bazel produced no shipped artifact")
            files = []
            for output in outputs:
                rel = Path(output)
                if rel.is_absolute() or ".." in rel.parts:
                    raise GateError(f"{root}: invalid Bazel output path: {output}")
                files += scan_artifact_output(ROOT / rel, root)
            if profile == "browser" and (not any(row["path"].endswith(".wasm") for row in files) or
                                         not any(row["path"].endswith(".js") for row in files)):
                raise GateError(f"{root}: shipped browser package lacks Wasm or JS")
            receipts.append({"profile": profile, "root": root, "files": files})
    return receipts


def platform_receipt(os_name: str, bazel: str) -> dict[str, Any]:
    actual = "macos" if sys.platform == "darwin" else "linux" if sys.platform.startswith("linux") else "other"
    if os_name != actual:
        raise GateError(f"requested {os_name} closure on {actual} host")
    spec = inventory()
    pins = archive_pins()
    # On Linux, loading the one test oracle fetches a checksum-verified archive
    # and materializes the extension's generated repo specs in the lock. macOS
    # must not need any wgpu-native archive after the product cutover.
    lock_sha = None
    closures = []
    for profile, roots in spec["platforms"][os_name].items():
        for root in roots:
            labels = query_closure(root, profile, bazel)
            check_closure(root, profile, labels, spec)
            closures.append({"profile": profile, "root": root, "labelsSha256": digest(canonical(sorted(labels))),
                             "labelCount": len(labels)})
    cmake = check_cmake_consumer() if os_name == "linux" else None
    artifacts = build_and_scan_artifacts(os_name, bazel, spec)
    if os_name == "linux":
        lock_sha = verify_lock(pins)
    return {"schema": SCHEMA, **source_identity(), "platform": os_name,
            "architecture": platform.machine().lower(), "lockSha256": lock_sha,
            "pins": pins, "closures": closures, "cmake": cmake, "artifacts": artifacts}


def verify_receipts(receipts: list[dict[str, Any]]) -> None:
    spec = inventory()
    expected = source_identity()
    if len(receipts) != 2 or {r.get("platform") for r in receipts} != {"linux", "macos"}:
        raise GateError("both Linux and macOS configured closure receipts are required")
    pins = archive_pins()
    for receipt in receipts:
        os_name = receipt["platform"]
        if receipt.get("schema") != SCHEMA or any(receipt.get(key) != value for key, value in expected.items()):
            raise GateError(f"{os_name}: stale or mismatched source/inventory receipt")
        if receipt.get("pins") != pins or (os_name == "linux" and not re.fullmatch(r"[0-9a-f]{64}", receipt.get("lockSha256", ""))) or (os_name == "macos" and receipt.get("lockSha256") is not None):
            raise GateError(f"{os_name}: stale or mismatched archive lock receipt")
        if not receipt.get("architecture"):
            raise GateError(f"{os_name}: missing host architecture")
        expected_pairs = {(profile, root) for profile, roots in spec["platforms"][os_name].items() for root in roots}
        closures = receipt.get("closures", [])
        actual_pairs = {(row.get("profile"), row.get("root")) for row in closures}
        if len(closures) != len(expected_pairs) or actual_pairs != expected_pairs:
            raise GateError(f"{os_name}: missing, duplicate, or unexpected configured root receipt")
        if any(not re.fullmatch(r"[0-9a-f]{64}", row.get("labelsSha256", "")) or row.get("labelCount", 0) < 2 for row in closures):
            raise GateError(f"{os_name}: empty or malformed closure receipt")
        if os_name == "linux":
            cmake = receipt.get("cmake")
            if not isinstance(cmake, dict) or cmake.get("installSurface") != "absent" or \
               cmake.get("consumerPassed") is not True or \
               not re.fullmatch(r"[0-9a-f]{64}", cmake.get("generatedCmakeSha256", "")):
                raise GateError("Linux generated CMake/no-install consumer receipt is missing or malformed")
        elif receipt.get("cmake") is not None:
            raise GateError("unexpected macOS CMake receipt")
        artifact_roots = spec["artifactRoots"][os_name]
        expected_artifacts = {(profile, root) for profile, roots in artifact_roots.items() for root in roots}
        artifacts = receipt.get("artifacts", [])
        if not isinstance(artifacts, list) or len(artifacts) != len(expected_artifacts) or \
           {(row.get("profile"), row.get("root")) for row in artifacts} != expected_artifacts:
            raise GateError(f"{os_name}: missing or unexpected shipped Bazel artifact receipt")
        for artifact in artifacts:
            files = artifact.get("files", [])
            if not isinstance(files, list) or not files or any(
                not isinstance(item, dict) or not isinstance(item.get("path"), str) or
                not isinstance(item.get("bytes"), int) or item["bytes"] <= 0 or
                not re.fullmatch(r"[0-9a-f]{64}", item.get("sha256", "")) or
                BAD_ARTIFACT_RE.search(item["path"])
                for item in files
            ):
                raise GateError(f"{os_name}: malformed or contaminated shipped artifact receipt")
            if len({item["path"] for item in files}) != len(files):
                raise GateError(f"{os_name}: duplicate shipped artifact path")
            if artifact["profile"] == "browser" and (
                not any(item["path"].endswith(".wasm") for item in files) or
                not any(item["path"].endswith(".js") for item in files)
            ):
                raise GateError(f"{os_name}: shipped browser receipt lacks Wasm or JS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    scan = sub.add_parser("scan")
    scan.add_argument("--platform", choices=("linux", "macos"), required=True)
    scan.add_argument("--bazel", default="bazel")
    scan.add_argument("--output", type=Path, required=True)
    aggregate = sub.add_parser("aggregate")
    aggregate.add_argument("receipts", nargs="+", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "scan":
            result = platform_receipt(args.platform, args.bazel)
            args.output.write_text(json.dumps(result, sort_keys=True, indent=2) + "\n")
            print(f"PASS: {args.platform} configured no-Rust closures ({len(result['closures'])} roots)")
        else:
            verify_receipts([json.loads(path.read_text()) for path in args.receipts])
            print("PASS: Linux and macOS configured no-Rust closure receipts match this source and lock")
    except (GateError, OSError, ValueError, KeyError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
