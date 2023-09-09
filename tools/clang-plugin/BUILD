load("@bazel_skylib//rules:common_settings.bzl", "bool_setting")
load("@rules_cc//cc:defs.bzl", "cc_binary")
load(":libclang_cc_binary.bzl", "libclang_cc_binary", "require_libclang")

bool_setting(
    name = "setting_libclang_build",
    build_setting_default = False,
)

config_setting(
    name = "libclang_build",
    flag_values = {
        ":setting_libclang_build": "true",
    },
)

cc_binary(
    name = "clang-plugin-bin",
    srcs = [
        "main.cc",
    ],
    includes = ["src"],
    target_compatible_with = require_libclang(),
    deps = [
        "@llvm_toolchain_llvm//:lib_clangAST",
        "@llvm_toolchain_llvm//:lib_clangBasic",
        "@llvm_toolchain_llvm//:lib_clangFrontend",
        "@llvm_toolchain_llvm//:lib_clangIndex",
        "@llvm_toolchain_llvm//:lib_clangTooling",
        "@llvm_toolchain_llvm//:lib_clangToolingCore",
        "@llvm_toolchain_llvm//:lib_clangToolingInclusions",
        "@llvm_toolchain_llvm//:libclang_include",
    ],
)

libclang_cc_binary(
    name = "clang-plugin",
    actual_binary = ":clang-plugin-bin",
    visibility = ["//visibility:public"],
)
