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
    name = "pathfinder_content",
    crate_root = "content/src/lib.rs",
    crate_type = "lib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__arrayvec__0_5_1//:arrayvec",
        "@raze__bitflags__1_2_1//:bitflags",
        "@raze__image__0_23_4//:image",
        "@raze__log__0_4_8//:log",
        "@raze__pathfinder_color__0_5_0//:pathfinder_color",
        "@raze__pathfinder_geometry__0_5_1//:pathfinder_geometry",
        "@raze__pathfinder_simd__0_5_0//:pathfinder_simd",
        "@raze__smallvec__1_4_0//:smallvec",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.5.0",
    crate_features = [
        "default",
        "image",
        "pf-image",
    ],
)

