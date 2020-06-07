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
    name = "pathfinder_renderer",
    crate_root = "renderer/src/lib.rs",
    crate_type = "lib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__bitflags__1_2_1//:bitflags",
        "@raze__byteorder__1_3_4//:byteorder",
        "@raze__crossbeam_channel__0_4_2//:crossbeam_channel",
        "@raze__fxhash__0_2_1//:fxhash",
        "@raze__half__1_6_0//:half",
        "@raze__hashbrown__0_7_2//:hashbrown",
        "@raze__instant__0_1_4//:instant",
        "@raze__log__0_4_8//:log",
        "@raze__pathfinder_color__0_5_0//:pathfinder_color",
        "@raze__pathfinder_content__0_5_0//:pathfinder_content",
        "@raze__pathfinder_geometry__0_5_1//:pathfinder_geometry",
        "@raze__pathfinder_gpu__0_5_0//:pathfinder_gpu",
        "@raze__pathfinder_resources__0_5_0//:pathfinder_resources",
        "@raze__pathfinder_simd__0_5_0//:pathfinder_simd",
        "@raze__pathfinder_ui__0_5_0//:pathfinder_ui",
        "@raze__rayon__1_3_0//:rayon",
        "@raze__serde__1_0_111//:serde",
        "@raze__serde_json__1_0_53//:serde_json",
        "@raze__smallvec__1_4_0//:smallvec",
        "@raze__vec_map__0_8_2//:vec_map",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.5.0",
    crate_features = [
    ],
)

