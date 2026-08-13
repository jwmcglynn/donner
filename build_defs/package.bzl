"""Per-package boilerplate for repo-wide rules.

`donner_package()` is the single place a BUILD file opts into the rules that
apply to every package. Today that is one thing - exporting the package's
sources to the GPU inventory ratchet - and the call site is one line:

    load("//build_defs:package.bzl", "donner_package")

    donner_package()

The indirection exists so repo-wide machinery lands here instead of being
copy-pasted into every BUILD file. When the next global rule arrives (a
per-package lint target, a doxygen coverage verifier), it becomes a keyword
argument on this macro and every package picks it up without touching 60 BUILD
files. Nothing but the source export is wired up yet; this docstring is the
contract for whoever adds the second one.

## Why a per-package declaration at all

Bazel's `glob()` is package-local by construction: it stops at any directory
containing a BUILD file, because those files belong to that package's targets,
not this one's. A `genrule` cannot escape the boundary either - its inputs must
be labels, so it can only name sources some package already exposes. In a repo
with ~60 packages there is no mechanism that lets one target enumerate every
source file without each package saying so. Hence this macro: the declaration is
unavoidable, but it does not have to be verbose.

Forgetting the call is not silent. The GPU inventory freshness test asserts that
every file named in the checked-in manifests is one of its declared inputs, so a
package that gains GPU code but not a `donner_package()` fails loudly instead of
being quietly skipped. See tools/gpu_inventory/README.md.
"""

# File types that feed the GPU inventory scan (tools/gpu_inventory). Kept
# deliberately wide: the scan is lexical and over-approximating, so a file type
# that *could* mention a GPU token belongs here even if none currently do.
#
# `**` patterns are intentional. They collect files in plain subdirectories of
# the package while still stopping at subpackages, which declare their own
# `donner_package()`.
_DEFAULT_GPU_INVENTORY_GLOBS = [
    "**/*.h",
    "**/*.cc",
    "**/*.mm",
    "**/*.wgsl",
    "**/*.patch",
    "**/*.bzl",
    "BUILD.bazel",
]

# Directories that are build output or fetched dependencies, not package
# sources. Bazel's convenience symlinks (`bazel-bin`, `bazel-out`,
# `bazel-testlogs`, `bazel-<workspace>`) and `external` live in the workspace
# root and `**` globs follow them, so without this the root package's glob walks
# the whole output base. It re-collects staged copies of real sources under
# runfiles paths, and because runfiles trees nest, it collects the same file at
# exponentially many depths: measured at 27010 matches against 1353 real files.
_OUTPUT_TREE_EXCLUDES = [
    "bazel-*/**",
    "external/**",
    "**/*.runfiles/**",
]

def donner_package(gpu_inventory_globs = None):
    """Declares the repo-wide per-package rules for this BUILD file.

    Args:
      gpu_inventory_globs: Replaces the default glob list for the GPU inventory
        export. Only the handful of packages that are not first-party C++ pass
        this - the repo root and `third_party/*`, which hold build-graph files
        the defaults do not name (`MODULE.bazel`, the platform BUILD overlays)
        and vendored trees that are deliberately NOT scanned wholesale. Every
        first-party package passes nothing.

        Keep prose in this file free of the tokens the inventory scan matches.
        The scan is lexical and deliberately over-approximating, so it reads
        comments too: naming a WebGPU archive overlay verbatim here registered
        this file as a new Rust-built archive site and failed the ratchet, which
        is the scan working correctly on the wrong input.
    """
    native.filegroup(
        name = "gpu_inventory_srcs",
        srcs = native.glob(
            gpu_inventory_globs if gpu_inventory_globs != None else _DEFAULT_GPU_INVENTORY_GLOBS,
            exclude = _OUTPUT_TREE_EXCLUDES,
            allow_empty = True,
        ),
        visibility = ["//tools/gpu_inventory:__pkg__"],
    )
