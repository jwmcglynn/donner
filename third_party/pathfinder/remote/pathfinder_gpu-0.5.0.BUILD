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
    name = "pathfinder_gpu",
    crate_root = "gpu/src/lib.rs",
    crate_type = "lib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__bitflags__1_2_1//:bitflags",
        "@raze__half__1_6_0//:half",
        "@raze__image__0_23_4//:image",
        "@raze__pathfinder_color__0_5_0//:pathfinder_color",
        "@raze__pathfinder_geometry__0_5_1//:pathfinder_geometry",
        "@raze__pathfinder_resources__0_5_0//:pathfinder_resources",
        "@raze__pathfinder_simd__0_5_0//:pathfinder_simd",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.5.0",
    crate_features = [
    ],
)

