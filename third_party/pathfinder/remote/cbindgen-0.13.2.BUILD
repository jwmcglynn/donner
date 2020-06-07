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
  "reciprocal", # "MPL-2.0"
])

load(
    "@io_bazel_rules_rust//rust:rust.bzl",
    "rust_library",
    "rust_binary",
    "rust_test",
)


# Unsupported target "build-script-build" with type "custom-build" omitted
rust_binary(
    # Prefix bin name to disambiguate from (probable) collision with lib name
    # N.B.: The exact form of this is subject to change.
    name = "cargo_bin_cbindgen",
    crate_root = "src/main.rs",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        # Binaries get an implicit dependency on their crate's lib
        ":cbindgen",
        "@raze__clap__2_33_1//:clap",
        "@raze__log__0_4_8//:log",
        "@raze__proc_macro2__1_0_18//:proc_macro2",
        "@raze__quote__1_0_6//:quote",
        "@raze__serde__1_0_111//:serde",
        "@raze__serde_json__1_0_53//:serde_json",
        "@raze__syn__1_0_30//:syn",
        "@raze__tempfile__3_1_0//:tempfile",
        "@raze__toml__0_5_6//:toml",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.13.2",
    crate_features = [
        "clap",
        "default",
    ],
)


rust_library(
    name = "cbindgen",
    crate_root = "src/lib.rs",
    crate_type = "lib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__clap__2_33_1//:clap",
        "@raze__log__0_4_8//:log",
        "@raze__proc_macro2__1_0_18//:proc_macro2",
        "@raze__quote__1_0_6//:quote",
        "@raze__serde__1_0_111//:serde",
        "@raze__serde_json__1_0_53//:serde_json",
        "@raze__syn__1_0_30//:syn",
        "@raze__tempfile__3_1_0//:tempfile",
        "@raze__toml__0_5_6//:toml",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.13.2",
    crate_features = [
        "clap",
        "default",
    ],
)

# Unsupported target "tests" with type "test" omitted
