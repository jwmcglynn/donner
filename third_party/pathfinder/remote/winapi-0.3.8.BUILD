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


# Unsupported target "build-script-build" with type "custom-build" omitted

rust_library(
    name = "winapi",
    crate_root = "src/lib.rs",
    crate_type = "lib",
    edition = "2015",
    srcs = glob(["**/*.rs"]),
    deps = [
    ],
    rustc_flags = [
        "--cap-lints=allow",
    ],
    version = "0.3.8",
    crate_features = [
        "consoleapi",
        "dwrite",
        "dwrite_1",
        "dwrite_3",
        "errhandlingapi",
        "fileapi",
        "handleapi",
        "knownfolders",
        "libloaderapi",
        "minwinbase",
        "minwindef",
        "objbase",
        "processenv",
        "processthreadsapi",
        "shlobj",
        "std",
        "sysinfoapi",
        "unknwnbase",
        "winbase",
        "wincon",
        "winerror",
        "winnls",
        "winnt",
    ],
)

