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
  "notice", # "BSD-3-Clause"
])

load(
    "@io_bazel_rules_rust//rust:rust.bzl",
    "rust_library",
    "rust_binary",
    "rust_test",
)


# Unsupported target "instant" with type "cdylib" omitted

rust_library(
    name = "instant",
    crate_root = "src/lib.rs",
    crate_type = "rlib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.1.4",
    crate_features = [
        "js-sys",
        "wasm-bindgen",
        "wasm-bindgen_rs",
        "web-sys",
    ],
)

# Unsupported target "wasm" with type "test" omitted
