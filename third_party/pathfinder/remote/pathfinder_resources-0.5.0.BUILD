"""
cargo-raze crate build file.

DO NOT EDIT! Replaced on runs of cargo-raze
"""
package(default_visibility = [
  # Public for visibility by "@raze__crate__version//" targets.
  #
  # Prefer access through "//third_party/pathfinder", which limits external
  # visibility to explicit Cargo.toml dependencies.
  "//visibility:public",
])

licenses([
  "notice", # "MIT,Apache-2.0"
])

load(
    "@io_bazel_rules_rust//rust:rust.bzl",
    "rust_library",
    "rust_binary",
    "rust_test",
)

rust_binary(
    name = "pathfinder_resources_build_script",
    srcs = glob(["**/*.rs"]),
    crate_root = "resources/build.rs",
    edition = "2018",
    deps = [
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    crate_features = [
    ],
    data = glob(["*"]),
    version = "0.5.0",
    visibility = ["//visibility:private"],
)

genrule(
    name = "pathfinder_resources_build_script_executor",
    srcs = glob(["resources/debug-fonts/*", "resources/shaders/**/*", "resources/textures/*"]) + ["resources/MANIFEST", "resources/Cargo.toml"],
    outs = ["pathfinder_resources_out_dir_outputs.tar.gz"],
    tools = [
      ":pathfinder_resources_build_script",
    ],
    tags = ["no-sandbox"],
    cmd = "mkdir -p $$(dirname $@)/pathfinder_resources_out_dir_outputs/;"
        + " (export CARGO_MANIFEST_DIR=\"$$PWD/$$(dirname $(location :resources/Cargo.toml))\";"
        # TODO(acmcarther): This needs to be revisited as part of the cross compilation story.
        #                   See also: https://github.com/google/cargo-raze/pull/54
        + " export TARGET='x86_64-unknown-linux-gnu';"
        + " export RUST_BACKTRACE=1;"
        + " export OUT_DIR=$$PWD/$$(dirname $@)/pathfinder_resources_out_dir_outputs;"
        + " export BINARY_PATH=\"$$PWD/$(location :pathfinder_resources_build_script)\";"
        + " export OUT_TAR=$$PWD/$@;"
        + " cd $$(dirname $(location :resources/Cargo.toml)) && $$BINARY_PATH && tar -czf $$OUT_TAR -C $$OUT_DIR .)"
)


rust_library(
    name = "pathfinder_resources",
    crate_root = "resources/src/lib.rs",
    crate_type = "lib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    out_dir_tar = ":pathfinder_resources_build_script_executor",
    version = "0.5.0",
    crate_features = [
    ],
)

