# C++26 reflection prototype

A standalone Bazel module for GCC 16 reflection. Run commands from this directory.
It does not change Donner's C++20 toolchain or link against Donner.

## Linux x86-64

The `gcc16` configuration downloads GCC 16.2.0 and its sysroot through
[`gcc_toolchain` 0.12.0](https://registry.bazel.build/modules/gcc_toolchain/0.12.0/).
The upstream repository rule verifies SHA-256 checksums.

```sh
bazel run --config=gcc16 //:reflection_demo
bazel test --config=gcc16 //:reflection_tests
```

## Native macOS (Apple Silicon)

Install Homebrew GCC 16, then use the local GCC configuration:

```sh
HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1 brew install gcc
bazel run --config=macos-gcc16 //:reflection_demo
bazel test --config=macos-gcc16 //:reflection_tests
```

This path uses GCC from `PATH`, Homebrew's libstdc++, and the macOS SDK. It is
not hermetic. The configuration bypasses the Apple Clang toolchain, selects
libstdc++, and disables two Clang-specific `rules_cc` features (reproducible
debug paths and automatic Objective-C runtime linkage). It is intended for
this C++-only prototype, not Donner's Objective-C++ editor targets.

Validated with Homebrew GCC 16.2.0 on Apple Silicon macOS 26.2. The supplied
configuration targets macOS 26.0 to match that Homebrew bottle's runtime;
older macOS versions and Intel Macs have not been tested. A different bottle
may require an explicit `--macos_minimum_os` value.

The downloaded Linux Bazel GCC package cannot execute on macOS. Its AArch64
variant is a Linux cross-compiler running on x86-64, not an ARM64 host
compiler. A Mac can also drive a separately configured Linux x86-64 remote
execution platform, producing Linux binaries; that route is not configured
by this experiment. Apple Clang cannot compile the reflection source.

## What is validated

The demo prints compiler/feature versions, discovers four rectangle fields,
mutates them through spliced member access, and identifies an enum value.
Compile-time assertions check member count, name, type, and type splicing.
Five GoogleTest cases check order and values, mutation, heterogeneous and empty
types, and unknown enum values. No code generator or registration list is used.

Both configurations disable exceptions and RTTI. Expansion statements use a
`static constexpr` member range, giving it the constant address required by GCC.

Expected demo output (the compiler banner may include packaging details):

```text
GCC 16.2.0
__cpp_impl_reflection=202603
x=1, y=2, width=30, height=40
After reflected mutation: x=2, y=3, width=31, height=41
Paint mode: Stroke
Reflection validation passed
```

## Scope

This experiment proves the reflection features exercised here, not full C++26
conformance or that all of Donner builds with GCC. The standalone module is
excluded from the parent workspace's wildcard builds. Keep Bazel's output base
outside the source checkout.
