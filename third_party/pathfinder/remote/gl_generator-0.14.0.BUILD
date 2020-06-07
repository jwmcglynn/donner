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
  "notice", # "Apache-2.0"
])

load(
    "@io_bazel_rules_rust//rust:rust.bzl",
    "rust_library",
    "rust_binary",
    "rust_test",
)



rust_library(
    name = "gl_generator",
    crate_root = "lib.rs",
    crate_type = "lib",
    edition = "2015",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__khronos_api__3_1_0//:khronos_api",
        "@raze__log__0_4_8//:log",
        "@raze__xml_rs__0_8_3//:xml_rs",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.14.0",
    crate_features = [
    ],
)

