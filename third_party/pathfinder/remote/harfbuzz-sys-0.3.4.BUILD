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
])

load(
    "@io_bazel_rules_rust//rust:rust.bzl",
    "rust_library",
    "rust_binary",
    "rust_test",
)


# Unsupported target "build-script-build" with type "custom-build" omitted

rust_library(
    name = "harfbuzz_sys",
    crate_root = "src/lib.rs",
    crate_type = "lib",
    edition = "2015",
    srcs = glob(["**/*.rs"]),
    deps = [
        "@raze__freetype__0_4_1//:freetype",
        ":harfbuzz",
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.3.4",
    crate_features = [
        "build-native-freetype",
        "build-native-harfbuzz",
        "cc",
        "default",
        "pkg-config",
    ],
)


# Additional content from harfbuzz_sys_patch.BUILD
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "harfbuzz",
    srcs = [
        "harfbuzz/src/harfbuzz.cc",
    ],
    hdrs = glob([
        "harfbuzz/src/*.cc",
        "harfbuzz/src/*.hh",
        "harfbuzz/src/*.h",
    ]),
    defines = [
        "HAVE_PTHREAD=1",
    ],
)
