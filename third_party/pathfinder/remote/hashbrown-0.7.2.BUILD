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


# Unsupported target "bench" with type "bench" omitted
# Unsupported target "build-script-build" with type "custom-build" omitted

rust_library(
    name = "hashbrown",
    crate_root = "src/lib.rs",
    crate_type = "lib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__ahash__0_3_5//:ahash",
        "@raze__serde__1_0_111//:serde",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.7.2",
    crate_features = [
        "ahash",
        "default",
        "inline-more",
        "serde",
    ],
)

# Unsupported target "hasher" with type "test" omitted
# Unsupported target "rayon" with type "test" omitted
# Unsupported target "serde" with type "test" omitted
# Unsupported target "set" with type "test" omitted
