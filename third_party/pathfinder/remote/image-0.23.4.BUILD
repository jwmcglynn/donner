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


# Unsupported target "decode" with type "bench" omitted
# Unsupported target "encode" with type "bench" omitted

rust_library(
    name = "image",
    crate_root = "src/lib.rs",
    crate_type = "lib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__bytemuck__1_2_0//:bytemuck",
        "@raze__byteorder__1_3_4//:byteorder",
        "@raze__num_iter__0_1_40//:num_iter",
        "@raze__num_rational__0_2_4//:num_rational",
        "@raze__num_traits__0_2_11//:num_traits",
        "@raze__png__0_16_4//:png",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    data = ["README.md"],
    version = "0.23.4",
    crate_features = [
        "png",
    ],
)

