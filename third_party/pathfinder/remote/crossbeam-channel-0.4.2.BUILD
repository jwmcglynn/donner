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
  "restricted", # "Apache-2.0 AND BSD-2-Clause"
])

load(
    "@io_bazel_rules_rust//rust:rust.bzl",
    "rust_library",
    "rust_binary",
    "rust_test",
)


# Unsupported target "after" with type "test" omitted
# Unsupported target "array" with type "test" omitted
# Unsupported target "crossbeam" with type "bench" omitted

rust_library(
    name = "crossbeam_channel",
    crate_root = "src/lib.rs",
    crate_type = "lib",
    edition = "2015",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__crossbeam_utils__0_7_2//:crossbeam_utils",
        "@raze__maybe_uninit__2_0_0//:maybe_uninit",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.4.2",
    crate_features = [
    ],
)

# Unsupported target "fibonacci" with type "example" omitted
# Unsupported target "golang" with type "test" omitted
# Unsupported target "iter" with type "test" omitted
# Unsupported target "list" with type "test" omitted
# Unsupported target "matching" with type "example" omitted
# Unsupported target "mpsc" with type "test" omitted
# Unsupported target "never" with type "test" omitted
# Unsupported target "ready" with type "test" omitted
# Unsupported target "same_channel" with type "test" omitted
# Unsupported target "select" with type "test" omitted
# Unsupported target "select_macro" with type "test" omitted
# Unsupported target "stopwatch" with type "example" omitted
# Unsupported target "thread_locals" with type "test" omitted
# Unsupported target "tick" with type "test" omitted
# Unsupported target "zero" with type "test" omitted
