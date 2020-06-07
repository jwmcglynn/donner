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
    name = "pathfinder_metal",
    crate_root = "metal/src/lib.rs",
    crate_type = "lib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__bitflags__1_2_1//:bitflags",
        "@raze__block__0_1_6//:block",
        "@raze__byteorder__1_3_4//:byteorder",
        "@raze__cocoa__0_19_1//:cocoa",
        "@raze__core_foundation__0_6_4//:core_foundation",
        "@raze__foreign_types__0_3_2//:foreign_types",
        "@raze__half__1_6_0//:half",
        "@raze__io_surface__0_12_1//:io_surface",
        "@raze__libc__0_2_71//:libc",
        "@raze__metal__0_17_1//:metal",
        "@raze__objc__0_2_7//:objc",
        "@raze__pathfinder_geometry__0_5_1//:pathfinder_geometry",
        "@raze__pathfinder_gpu__0_5_0//:pathfinder_gpu",
        "@raze__pathfinder_resources__0_5_0//:pathfinder_resources",
        "@raze__pathfinder_simd__0_5_0//:pathfinder_simd",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.5.1",
    crate_features = [
    ],
)

