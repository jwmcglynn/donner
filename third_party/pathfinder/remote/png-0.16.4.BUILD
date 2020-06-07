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
  "restricted", # "MIT OR Apache-2.0"
])

load(
    "@io_bazel_rules_rust//rust:rust.bzl",
    "rust_library",
    "rust_binary",
    "rust_test",
)


# Unsupported target "decoder" with type "bench" omitted

rust_library(
    name = "png",
    crate_root = "src/lib.rs",
    crate_type = "lib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__bitflags__1_2_1//:bitflags",
        "@raze__crc32fast__1_2_0//:crc32fast",
        "@raze__deflate__0_8_4//:deflate",
        "@raze__inflate__0_4_5//:inflate",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.16.4",
    crate_features = [
        "default",
        "deflate",
        "png-encoding",
    ],
)

# Unsupported target "pngcheck" with type "example" omitted
# Unsupported target "show" with type "example" omitted
