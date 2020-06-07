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


# Unsupported target "argument-buffer" with type "example" omitted
# Unsupported target "bind" with type "example" omitted
# Unsupported target "caps" with type "example" omitted
# Unsupported target "compute" with type "example" omitted
# Unsupported target "compute-argument-buffer" with type "example" omitted
# Unsupported target "embedded-lib" with type "example" omitted
# Unsupported target "library" with type "example" omitted

rust_library(
    name = "metal",
    crate_root = "src/lib.rs",
    crate_type = "lib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__bitflags__1_2_1//:bitflags",
        "@raze__block__0_1_6//:block",
        "@raze__cocoa__0_19_1//:cocoa",
        "@raze__core_graphics__0_17_3//:core_graphics",
        "@raze__foreign_types__0_3_2//:foreign_types",
        "@raze__log__0_4_8//:log",
        "@raze__objc__0_2_7//:objc",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.17.1",
    crate_features = [
        "default",
    ],
)

# Unsupported target "reflection" with type "example" omitted
# Unsupported target "window" with type "example" omitted
