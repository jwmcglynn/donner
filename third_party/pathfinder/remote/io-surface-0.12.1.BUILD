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
    name = "io_surface",
    crate_root = "src/lib.rs",
    crate_type = "lib",
    edition = "2015",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__cgl__0_3_2//:cgl",
        "@raze__core_foundation__0_6_4//:core_foundation",
        "@raze__gleam__0_7_0//:gleam",
        "@raze__leaky_cow__0_1_1//:leaky_cow",
        "@raze__libc__0_2_71//:libc",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.12.1",
    crate_features = [
    ],
)

