load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

# Apply AVX2+FMA on x86 in native mode; suppressed in scalar mode.
# simd_native_x86_* (flag + constraint, 2 conditions) is more specific than
# simd_scalar (flag only, 1 condition), so Bazel resolves unambiguously.
_SIMD_NATIVE_X86_COPTS = select({
    "//bazel/config:simd_native_x86_64": ["-mavx2", "-mfma"],
    "//bazel/config:simd_native_x86_32": ["-mavx2", "-mfma"],
    "//conditions:default": [],
})

_OPT_MODE_COPTS = select({
    "//bazel/config:compilation_mode_opt": ["-O3"],
    "//conditions:default": [],
})

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
        copts = ["-std=c++20", "-ffp-contract=off", "-Wall"] + _OPT_MODE_COPTS + _SIMD_NATIVE_X86_COPTS + copts,
        defines = defines,
        visibility = visibility,
        **kwargs
    )

def tiny_skia_cc_binary(name, srcs, deps = [], copts = [], **kwargs):
    cc_binary(
        name = name,
        srcs = srcs,
        deps = deps,
        copts = ["-std=c++20"] + _SIMD_NATIVE_X86_COPTS + copts,
        **kwargs
    )
