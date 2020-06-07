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
    name = "wasm_bindgen_backend",
    crate_root = "src/lib.rs",
    crate_type = "lib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__bumpalo__3_4_0//:bumpalo",
        "@raze__lazy_static__1_4_0//:lazy_static",
        "@raze__log__0_4_8//:log",
        "@raze__proc_macro2__1_0_18//:proc_macro2",
        "@raze__quote__1_0_6//:quote",
        "@raze__syn__1_0_30//:syn",
        "@raze__wasm_bindgen_shared__0_2_63//:wasm_bindgen_shared",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.2.63",
    crate_features = [
        "spans",
    ],
)

