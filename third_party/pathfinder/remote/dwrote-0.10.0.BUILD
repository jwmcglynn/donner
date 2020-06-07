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
  "reciprocal", # "MPL-2.0"
])

load(
    "@io_bazel_rules_rust//rust:rust.bzl",
    "rust_library",
    "rust_binary",
    "rust_test",
)



rust_library(
    name = "dwrote",
    crate_root = "src/lib.rs",
    crate_type = "lib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__lazy_static__1_4_0//:lazy_static",
        "@raze__libc__0_2_71//:libc",
        "@raze__winapi__0_3_8//:winapi",
        "@raze__wio__0_2_2//:wio",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.10.0",
    crate_features = [
    ],
)

