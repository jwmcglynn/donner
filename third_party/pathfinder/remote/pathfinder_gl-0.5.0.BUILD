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
    name = "pathfinder_gl",
    crate_root = "gl/src/lib.rs",
    crate_type = "rlib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__gl__0_14_0//:gl",
        "@raze__half__1_6_0//:half",
        "@raze__log__0_4_8//:log",
        "@raze__pathfinder_geometry__0_5_1//:pathfinder_geometry",
        "@raze__pathfinder_gpu__0_5_0//:pathfinder_gpu",
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


rust_library(
    name = "pathfinder_gl_static",
    crate_root = "gl/src/lib.rs",
    crate_type = "staticlib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__gl__0_14_0//:gl",
        "@raze__half__1_6_0//:half",
        "@raze__log__0_4_8//:log",
        "@raze__pathfinder_geometry__0_5_1//:pathfinder_geometry",
        "@raze__pathfinder_gpu__0_5_0//:pathfinder_gpu",
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

