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



rust_library(
    name = "pathfinder_canvas",
    crate_root = "canvas/src/lib.rs",
    crate_type = "rlib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__font_kit__0_6_0//:font_kit",
        "@raze__pathfinder_color__0_5_0//:pathfinder_color",
        "@raze__pathfinder_content__0_5_0//:pathfinder_content",
        "@raze__pathfinder_geometry__0_5_1//:pathfinder_geometry",
        "@raze__pathfinder_renderer__0_5_0//:pathfinder_renderer",
        "@raze__pathfinder_text__0_5_0//:pathfinder_text",
        "@raze__skribo__0_1_0//:skribo",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.5.0",
    crate_features = [
        "font-kit",
        "pathfinder_text",
        "pf-text",
        "skribo",
    ],
)


rust_library(
    name = "pathfinder_canvas_static",
    crate_root = "canvas/src/lib.rs",
    crate_type = "staticlib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__font_kit__0_6_0//:font_kit",
        "@raze__pathfinder_color__0_5_0//:pathfinder_color",
        "@raze__pathfinder_content__0_5_0//:pathfinder_content",
        "@raze__pathfinder_geometry__0_5_1//:pathfinder_geometry",
        "@raze__pathfinder_renderer__0_5_0//:pathfinder_renderer",
        "@raze__pathfinder_text__0_5_0//:pathfinder_text",
        "@raze__skribo__0_1_0//:skribo",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.5.0",
    crate_features = [
        "font-kit",
        "pathfinder_text",
        "pf-text",
        "skribo",
    ],
)

