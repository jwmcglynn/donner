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
  "notice", # "MIT"
])

load(
    "@io_bazel_rules_rust//rust:rust.bzl",
    "rust_library",
    "rust_binary",
    "rust_test",
)


# Unsupported target "example" with type "example" omitted

rust_library(
    name = "objc",
    crate_root = "src/lib.rs",
    crate_type = "lib",
    edition = "2015",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__malloc_buf__0_0_6//:malloc_buf",
        "@raze__objc_exception__0_1_2//:objc_exception",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.2.7",
    crate_features = [
        "objc_exception",
    ],
)

