exports_files(glob(["*.bzl"]))

cc_library(
    name = "user_config",
    hdrs = ["SkUserConfig.h"],
    defines = [
        "SK_USE_BAZEL_CONFIG_HEADER",
    ],
    include_prefix = "include/config",
    visibility = ["//visibility:public"],
)
