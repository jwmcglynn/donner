load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

cc_library(
    name = "renderer_utils",
    srcs = ["renderer_utils.cc"],
    hdrs = ["renderer_utils.h"],
    deps = [
        "//src/svg",
        "@stb//:image_write",
    ],
)

cc_library(
    name = "renderer_skia",
    srcs = ["renderer_skia.cc"],
    hdrs = ["renderer_skia.h"],
    deps = [
        ":renderer_utils",
        "//src/svg",
        "//src/svg/core",
        "@skia//:skia_core",
    ],
)

cc_binary(
    name = "renderer_tool",
    srcs = ["renderer_tool.cc"],
    deps = [
        ":renderer_skia",
        "//src/svg/xml",
    ],
)
