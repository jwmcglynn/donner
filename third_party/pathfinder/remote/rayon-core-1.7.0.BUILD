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


# Unsupported target "build-script-build" with type "custom-build" omitted
# Unsupported target "double_init_fail" with type "test" omitted
# Unsupported target "init_zero_threads" with type "test" omitted

rust_library(
    name = "rayon_core",
    crate_root = "src/lib.rs",
    crate_type = "lib",
    edition = "2018",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__crossbeam_deque__0_7_3//:crossbeam_deque",
        "@raze__crossbeam_queue__0_2_2//:crossbeam_queue",
        "@raze__crossbeam_utils__0_7_2//:crossbeam_utils",
        "@raze__lazy_static__1_4_0//:lazy_static",
        "@raze__num_cpus__1_13_0//:num_cpus",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "1.7.0",
    crate_features = [
    ],
)

# Unsupported target "scope_join" with type "test" omitted
# Unsupported target "scoped_threadpool" with type "test" omitted
# Unsupported target "simple_panic" with type "test" omitted
# Unsupported target "stack_overflow_crash" with type "test" omitted
