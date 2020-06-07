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


# Unsupported target "alloc_fill" with type "test" omitted
# Unsupported target "alloc_with" with type "test" omitted
# Unsupported target "benches" with type "bench" omitted

rust_library(
    name = "bumpalo",
    crate_root = "src/lib.rs",
    crate_type = "lib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "3.4.0",
    crate_features = [
        "default",
    ],
)

# Unsupported target "quickchecks" with type "test" omitted
# Unsupported target "readme_up_to_date" with type "test" omitted
# Unsupported target "string" with type "test" omitted
# Unsupported target "tests" with type "test" omitted
# Unsupported target "try_alloc" with type "test" omitted
# Unsupported target "vec" with type "test" omitted
