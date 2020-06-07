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
    name = "pathfinder_ui",
    crate_root = "ui/src/lib.rs",
    crate_type = "lib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__hashbrown__0_7_2//:hashbrown",
        "@raze__pathfinder_color__0_5_0//:pathfinder_color",
        "@raze__pathfinder_geometry__0_5_1//:pathfinder_geometry",
        "@raze__pathfinder_gpu__0_5_0//:pathfinder_gpu",
        "@raze__pathfinder_resources__0_5_0//:pathfinder_resources",
        "@raze__pathfinder_simd__0_5_0//:pathfinder_simd",
        "@raze__serde__1_0_111//:serde",
        "@raze__serde_derive__1_0_111//:serde_derive",
        "@raze__serde_json__1_0_53//:serde_json",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.5.0",
    crate_features = [
    ],
)

