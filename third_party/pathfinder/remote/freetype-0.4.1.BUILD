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
  "notice", # "Apache-2.0,MIT"
])

load(
    "@io_bazel_rules_rust//rust:rust.bzl",
    "rust_library",
    "rust_binary",
    "rust_test",
)



rust_library(
    name = "freetype",
    crate_root = "src/lib.rs",
    crate_type = "rlib",
    edition = "2015",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__libc__0_2_71//:libc",
        "@raze__servo_freetype_sys__4_0_5//:servo_freetype_sys",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.4.1",
    crate_features = [
        "default",
        "servo-freetype-sys",
    ],
)

