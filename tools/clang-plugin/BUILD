load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "clang-plugin",
    srcs = [
        "main.cc",
    ],
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
