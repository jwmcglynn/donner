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
    name = "core_text",
    crate_root = "src/lib.rs",
    crate_type = "lib",
    edition = "2015",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__core_foundation__0_6_4//:core_foundation",
        "@raze__core_graphics__0_17_3//:core_graphics",
        "@raze__foreign_types__0_3_2//:foreign_types",
        "@raze__libc__0_2_71//:libc",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "13.3.2",
    crate_features = [
        "default",
        "mountainlion",
    ],
)

