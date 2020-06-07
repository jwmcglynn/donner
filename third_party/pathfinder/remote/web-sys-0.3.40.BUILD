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


# Unsupported target "wasm" with type "test" omitted

rust_library(
    name = "web_sys",
    crate_root = "src/lib.rs",
    crate_type = "lib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__js_sys__0_3_40//:js_sys",
        "@raze__wasm_bindgen__0_2_63//:wasm_bindgen",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.3.40",
    crate_features = [
        "EventTarget",
        "Performance",
        "PerformanceTiming",
        "Window",
    ],
)

