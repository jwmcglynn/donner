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


# Unsupported target "render" with type "example" omitted

rust_library(
    name = "skribo",
    crate_root = "src/lib.rs",
    crate_type = "lib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__font_kit__0_6_0//:font_kit",
        "@raze__harfbuzz__0_3_1//:harfbuzz",
        "@raze__harfbuzz_sys__0_3_4//:harfbuzz_sys",
        "@raze__log__0_4_8//:log",
        "@raze__pathfinder_geometry__0_5_1//:pathfinder_geometry",
        "@raze__unicode_normalization__0_1_12//:unicode_normalization",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.1.0",
    crate_features = [
    ],
)

