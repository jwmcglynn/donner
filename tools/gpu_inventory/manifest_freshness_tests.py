"""Enforces GPU inventory manifest freshness under plain `bazel test //...`.

This replaces the Lint workflow's `generate_gpu_manifests.py --check` step. The
scan inputs arrive as declared Bazel runfiles (the `gpu_inventory_srcs`
filegroups), never by walking the source tree and never by calling git, so the
check is an ordinary cacheable test: it re-runs when a scanned file changes and
is skipped otherwise, and change-based target selection can see why.

What it asserts is only that the checked-in manifests still equal a fresh scan
of those inputs. There is no content hash and no line count in the manifests, so
editing a GPU-using file without changing what it does with the GPU changes
nothing here and needs no regeneration.
"""

import json
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import generate_gpu_manifests as gen

WORKSPACE_NAME = "donner"
MANIFEST_RELDIR = "tools/gpu_inventory/manifests"
ALLOWLIST_RELPATH = "tools/gpu_inventory/rust_allowlist.json"
REGEN_COMMAND = "python3 tools/gpu_inventory/generate_gpu_manifests.py"

# Manifests whose entire scan universe can be declared as Bazel inputs, so they
# can be held to exact equality. `rust_dependencies.json` cannot: most of its
# content comes from `third_party/tiny-skia-cpp`, a vendored subtree listed in
# `.bazelignore` and therefore invisible to every target in this repo. It gets a
# one-directional ratchet instead; see the test below.
SOURCE_DERIVED_MANIFESTS = (
    "gpu_operations.json",
    "shader_features.json",
    "editor_integration.json",
)


def _runfiles_root() -> Path:
    """The workspace root inside the test's runfiles tree."""
    srcdir = os.environ.get("TEST_SRCDIR")
    if not srcdir:
        raise RuntimeError("TEST_SRCDIR is unset; this must run as a bazel test")
    for candidate in (Path(srcdir) / WORKSPACE_NAME, Path(srcdir) / "_main"):
        if candidate.is_dir():
            return candidate
    raise RuntimeError(f"no workspace directory under TEST_SRCDIR={srcdir}")


def _scanned_paths(root: Path) -> list[str]:
    """Repo-relative paths of every declared input that feeds a manifest scan.

    Walks the RUNFILES tree, which is the set of files this target declared, not
    the source tree. Anything missing here is missing a `gpu_inventory_srcs`
    filegroup, which `test_every_inventoried_file_is_a_declared_input` reports as
    a failure rather than silently under-scanning.
    """
    paths = []
    for dirpath, dirnames, filenames in os.walk(root, followlinks=True):
        dirnames[:] = [d for d in dirnames if not _is_not_a_source_dir(d)]
        for filename in filenames:
            relpath = os.path.relpath(os.path.join(dirpath, filename), root)
            if gen.is_scannable_path(relpath):
                paths.append(relpath)
    return sorted(paths)


def _is_not_a_source_dir(name: str) -> bool:
    """Directories that are build output, not declared source inputs.

    A runfiles tree can contain the runfiles of tools this target depends on,
    which are themselves staged copies of source files under `bazel-out/` and
    `*.runfiles/`. Walking into those double-counts real sources under fake
    paths - observed as `third_party/bazel/non_bcr_deps.bzl` reappearing as
    `bazel-out/.../gen_cmakelists_test.runfiles/_main/third_party/bazel/
    non_bcr_deps.bzl` and being reported as a brand-new Rust archive site.
    """
    return name in (".git", "external", "bazel-out") or name.endswith(".runfiles")


def _describe_entry_delta(path: str, expected, actual) -> list[str]:
    """Human-readable per-fact differences for one inventoried file."""
    if expected is None:
        return [f"  + {path}: newly inventoried -> {json.dumps(actual, sort_keys=True)}"]
    if actual is None:
        return [f"  - {path}: no longer inventoried (was {json.dumps(expected, sort_keys=True)})"]
    lines = []
    for key in sorted(set(expected) | set(actual)):
        before = expected.get(key)
        after = actual.get(key)
        if before == after:
            continue
        lines.append(
            f"  ~ {path}: {key}: {json.dumps(before, sort_keys=True)}"
            f" -> {json.dumps(after, sort_keys=True)}"
        )
    return lines


def _describe_manifest_delta(name: str, expected: dict, actual: dict) -> list[str]:
    """Names the file and the changed fact, not just 'the manifest is stale'."""
    lines = []
    for section in sorted(set(expected) | set(actual)):
        if section == "_comment":
            continue
        before = expected.get(section)
        after = actual.get(section)
        if before == after:
            continue
        if isinstance(before, dict) and isinstance(after, dict):
            for path in sorted(set(before) | set(after)):
                if before.get(path) == after.get(path):
                    continue
                lines.extend(
                    _describe_entry_delta(
                        f"{name}[{section}] {path}", before.get(path), after.get(path)
                    )
                )
        else:
            lines.append(
                f"  ~ {name}[{section}]: {json.dumps(before, sort_keys=True)}"
                f" -> {json.dumps(after, sort_keys=True)}"
            )
    return lines


class ManifestFreshnessTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = _runfiles_root()
        cls.paths = _scanned_paths(cls.root)
        cls.files = gen.collect_files(cls.root, cls.paths)
        cls.generated = gen.build_all_manifests(
            cls.files,
            gen.load_allowlist_prefixes(cls.root / ALLOWLIST_RELPATH),
        )

    def _checked_in(self, name):
        with open(self.root / MANIFEST_RELDIR / name, encoding="utf-8") as handle:
            return json.load(handle)

    def test_scan_inputs_are_present(self):
        """A missing filegroup would otherwise look like an empty, passing scan."""
        self.assertGreater(
            len(self.files),
            500,
            "the GPU inventory scan received almost no input files; a "
            "`gpu_inventory_srcs` filegroup is probably missing from a package",
        )

    def test_manifests_match_a_fresh_scan(self):
        failures = []
        for name in sorted(SOURCE_DERIVED_MANIFESTS):
            expected = self._checked_in(name)
            manifest = self.generated[name]
            if expected == manifest:
                continue
            failures.append(f"{name}:")
            failures.extend(_describe_manifest_delta(name, expected, manifest))
        self.assertEqual(
            [],
            failures,
            "GPU inventory manifests no longer describe the tree.\n"
            + "\n".join(failures)
            + f"\n\nIf these changes are intended, regenerate with:\n  {REGEN_COMMAND}",
        )

    def test_no_new_rust_edge_enters_the_build_graph(self):
        """One-directional ratchet on rust_dependencies.json.

        Full equality is not available for this manifest: most of its content is
        the vendored `third_party/tiny-skia-cpp` subtree, which `.bazelignore`
        hides from Bazel entirely, so those files cannot be declared as inputs
        to any target in this repo. What CAN be declared is every build-graph
        file Bazel does own - the root MODULE.bazel, build_defs, third_party
        overlays, and all of donner/ - and that is where a NEW Rust dependency
        would have to appear to affect this build.

        So this asserts the direction that matters: no Rust build edge or
        Rust-built archive site may show up outside the checked-in set. The
        vendored subtree's own inventory is covered by the regeneration command
        and by //tools/gpu_inventory:check_no_rust_dependencies_tests plus the
        Lint workflow's no-Rust-dependency verifier, which are unchanged.
        """
        expected = self._checked_in("rust_dependencies.json")
        actual = self.generated["rust_dependencies.json"]
        for section in ("rustBuildEdges", "rustBuiltArchiveSites"):
            new_paths = sorted(set(actual.get(section, {})) - set(expected.get(section, {})))
            self.assertEqual(
                [],
                new_paths,
                f"a new Rust dependency edge appeared in {section}: "
                f"{new_paths}. Design 0053 keeps Donner free of a Rust "
                f"toolchain dependency; if this edge is intended, regenerate "
                f"with:\n  {REGEN_COMMAND}",
            )
            for path, value in sorted(actual.get(section, {}).items()):
                if path in expected.get(section, {}):
                    self.assertEqual(
                        expected[section][path],
                        value,
                        f"{section}[{path}] changed; regenerate with:\n  {REGEN_COMMAND}",
                    )

    def test_every_inventoried_file_is_a_declared_input(self):
        """Closes the loop on filegroup coverage.

        If a manifest names a file this test cannot see, the scan is running
        against a smaller tree than the manifests describe - which would make a
        freshness pass meaningless for that file. That happens when a package
        gains GPU code but not a `gpu_inventory_srcs` filegroup.
        """
        scanned = set(self.paths)
        missing = []
        for name in ("gpu_operations.json", "editor_integration.json"):
            for path in self._checked_in(name).get("files", {}):
                if path not in scanned:
                    missing.append(f"{name}: {path}")
        for path in self._checked_in("gpu_operations.json").get("wgpuPatchFiles", {}):
            if path not in scanned:
                missing.append(f"gpu_operations.json: {path}")
        for shader in self._checked_in("shader_features.json").get("shaders", {}):
            path = shader.split("#", 1)[0]
            if path not in scanned:
                missing.append(f"shader_features.json: {path}")
        self.assertEqual(
            [],
            sorted(missing),
            "these inventoried files are not declared inputs of this test; add a "
            "`gpu_inventory_srcs` filegroup to their package",
        )

    def test_manifests_record_no_content_derived_bookkeeping(self):
        """The staleness mechanism must stay gone.

        Content hashes and line counts made every unrelated edit to a GPU file a
        manifest change, and the only fix was a mechanical regeneration commit.
        """
        for name in sorted(self.generated):
            blob = json.dumps(self._checked_in(name))
            for banned in ("sha256", "lineCount", "contentHash"):
                self.assertNotIn(
                    banned,
                    blob,
                    f"{name} records {banned}; the manifests hold semantic "
                    "inventory only",
                )


if __name__ == "__main__":
    unittest.main()
