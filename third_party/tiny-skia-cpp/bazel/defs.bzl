load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

# Apply AVX2+FMA on x86 in native mode; suppressed in scalar mode.
# simd_native_x86_* (flag + constraint, 2 conditions) is more specific than
# simd_scalar (flag only, 1 condition), so Bazel resolves unambiguously.
_SIMD_NATIVE_X86_COPTS = select({
    "//bazel/config:simd_native_x86_64": ["-mavx2", "-mfma"],
    "//bazel/config:simd_native_x86_32": ["-mavx2", "-mfma"],
    "//conditions:default": [],
})

_SIMD_NATIVE_WASM_COPTS = select({
    "//bazel/config:simd_native_wasm32": ["-msimd128"],
    "//conditions:default": [],
})

_FORCED_PERF_COPTS = [
    # tiny-skia is the default renderer hot path for Donner's editor, so keep
    # it optimized even when the top-level Bazel invocation is fastbuild/dbg.
    "-O3",
    "-DNDEBUG",
]

def tiny_skia_cc_library(
        name,
        srcs,
        hdrs,
        deps = [],
        copts = [],
        defines = [],
        visibility = ["//visibility:private"],
        **kwargs):
    cc_library(
        name = name,
        srcs = srcs,
        hdrs = hdrs,
        deps = deps,
        copts = (
            ["-std=c++20", "-ffp-contract=off", "-Wall"] +
            _FORCED_PERF_COPTS +
            _SIMD_NATIVE_X86_COPTS +
            _SIMD_NATIVE_WASM_COPTS +
            copts
        ),
        defines = defines,
        visibility = visibility,
        **kwargs
    )

def tiny_skia_cc_binary(name, srcs, deps = [], copts = [], **kwargs):
    cc_binary(
        name = name,
        srcs = srcs,
        deps = deps,
        copts = ["-std=c++20"] + _SIMD_NATIVE_X86_COPTS + _SIMD_NATIVE_WASM_COPTS + copts,
        **kwargs
    )
