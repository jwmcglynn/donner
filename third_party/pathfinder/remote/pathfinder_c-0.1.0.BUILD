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
  "restricted", # "no license"
])

load(
    "@io_bazel_rules_rust//rust:rust.bzl",
    "rust_library",
    "rust_binary",
    "rust_test",
)

rust_binary(
    name = "pathfinder_c_build_script",
    srcs = glob(["**/*.rs"]),
    crate_root = "c/build.rs",
    edition = "2018",
    deps = [
        "@raze__cbindgen__0_13_2//:cbindgen",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    crate_features = [
    ],
    data = glob(["*"]),
    version = "0.1.0",
    visibility = ["//visibility:private"],
)

genrule(
    name = "pathfinder_c_build_script_executor",
    srcs = glob(["*", "**/*.rs", "**/Cargo.toml"]),
    outs = ["pathfinder_c_out_dir_outputs.tar.gz"],
    tools = [
      ":pathfinder_c_build_script",
      "@rust_linux_x86_64//:cargo",
      "@rust_linux_x86_64//:rustc",
    ],
    tags = ["no-sandbox"],
    cmd = "mkdir -p $$(dirname $@)/pathfinder_c_out_dir_outputs/;"
        + " (export CARGO_MANIFEST_DIR=\"$$PWD/$$(dirname $(location :c/Cargo.toml))\";"
        # TODO(acmcarther): This needs to be revisited as part of the cross compilation story.
        #                   See also: https://github.com/google/cargo-raze/pull/54
        + " export TARGET='x86_64-unknown-linux-gnu';"
        + " export RUST_BACKTRACE=1;"
        + " export OUT_DIR=$$PWD/$$(dirname $@)/pathfinder_c_out_dir_outputs;"
        + " export BINARY_PATH=\"$$PWD/$(location :pathfinder_c_build_script)\";"
        + " export OUT_TAR=$$PWD/$@;"
        + " export CARGO=\"$$PWD/$(location @rust_linux_x86_64//:cargo)\";"
        + " export RUSTC=\"$$PWD/$(location @rust_linux_x86_64//:rustc)\";"
        + " cd $$(dirname $(location :c/Cargo.toml)) && $$BINARY_PATH && tar -czf $$OUT_TAR -C $$OUT_DIR .)",
)


rust_library(
    name = "pathfinder_c_static",
    crate_root = "c/src/lib.rs",
    crate_type = "staticlib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__font_kit__0_6_0//:font_kit",
        "@raze__foreign_types__0_3_2//:foreign_types",
        "@raze__gl__0_14_0//:gl",
        "@raze__libc__0_2_71//:libc",
        "@raze__pathfinder_canvas__0_5_0//:pathfinder_canvas",
        "@raze__pathfinder_color__0_5_0//:pathfinder_color",
        "@raze__pathfinder_content__0_5_0//:pathfinder_content",
        "@raze__pathfinder_geometry__0_5_1//:pathfinder_geometry",
        "@raze__pathfinder_gl__0_5_0//:pathfinder_gl",
        "@raze__pathfinder_gpu__0_5_0//:pathfinder_gpu",
        "@raze__pathfinder_renderer__0_5_0//:pathfinder_renderer",
        "@raze__pathfinder_resources__0_5_0//:pathfinder_resources",
        "@raze__pathfinder_simd__0_5_0//:pathfinder_simd",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    out_dir_tar = ":pathfinder_c_build_script_executor",
    version = "0.1.0",
    crate_features = [
    ],
)

