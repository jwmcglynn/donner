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


# Unsupported target "fallback" with type "example" omitted

rust_library(
    name = "font_kit",
    crate_root = "src/lib.rs",
    crate_type = "lib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__bitflags__1_2_1//:bitflags",
        "@raze__byteorder__1_3_4//:byteorder",
        "@raze__dirs__2_0_2//:dirs",
        "@raze__float_ord__0_2_0//:float_ord",
        "@raze__freetype__0_4_1//:freetype",
        "@raze__lazy_static__1_4_0//:lazy_static",
        "@raze__libc__0_2_71//:libc",
        "@raze__log__0_4_8//:log",
        "@raze__pathfinder_geometry__0_5_1//:pathfinder_geometry",
        "@raze__pathfinder_simd__0_5_0//:pathfinder_simd",
        "@raze__walkdir__2_3_1//:walkdir",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.6.0",
    crate_features = [
        "default",
        "source",
        "source-fs-default",
    ],
)

# Unsupported target "list-fonts" with type "example" omitted
# Unsupported target "match-font" with type "example" omitted
# Unsupported target "render-glyph" with type "example" omitted
# Unsupported target "select_font" with type "test" omitted
# Unsupported target "tests" with type "test" omitted
