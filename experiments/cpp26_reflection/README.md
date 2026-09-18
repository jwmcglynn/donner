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

The demo prints compiler/feature versions, discovers four rectangle fields,
mutates them through spliced member access, and identifies an enum value.
Compile-time assertions check member count, name, type, and type splicing.
Five GoogleTest cases check order and values, mutation, heterogeneous and empty
types, and unknown enum values. No code generator or registration list is used.

Both compilation modes disable exceptions and RTTI.

## macOS

The downloaded Bazel GCC package executes only on Linux x86-64. Its AArch64
variant is a cross-compiler, not an Apple Silicon or Linux ARM64 host compiler.
A Mac can use a suitably configured Linux x86-64 remote execution platform to
produce Linux binaries; those binaries do not run natively on macOS.

For a native Apple Silicon smoke test, Homebrew provides GCC 16.2:

```sh
brew install gcc
g++-16 -std=c++26 -freflection -fno-exceptions -fno-rtti \
  ReflectionDemo.cc -o /path/to/build/reflection_demo
/path/to/build/reflection_demo
```

Use a persistent build directory outside the source checkout. This native path
uses Homebrew and the macOS SDK; it is not the hermetic Bazel toolchain.
Apple Clang cannot compile this prototype.

## Scope

This experiment proves the reflection features exercised here, not full C++26
conformance or that all of Donner builds with GCC. The standalone module is
excluded from the parent workspace's wildcard builds.
