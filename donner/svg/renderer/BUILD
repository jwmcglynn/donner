load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")
load("//build_defs:rules.bzl", "donner_cc_library_perf_sensitive")

cc_library(
    name = "renderer_image_io",
    srcs = ["RendererImageIO.cc"],
    hdrs = ["RendererImageIO.h"],
    visibility = ["//donner/svg/renderer:__subpackages__"],
    deps = [
        "@stb//:image_write",
    ],
)

cc_library(
    name = "renderer_utils",
    srcs = ["RendererUtils.cc"],
    hdrs = ["RendererUtils.h"],
    visibility = ["//donner/svg:__subpackages__"],
    deps = [
        "//donner/svg",
    ],
)

cc_library(
    name = "rendering_context",
    srcs = ["RenderingContext.cc"],
    hdrs = ["RenderingContext.h"],
    visibility = ["//donner/svg:__subpackages__"],
    deps = [
        "//donner/svg/components",
    ],
)

donner_cc_library_perf_sensitive(
    name = "skia_deps",
    deps = [
        "@skia//:core",
        "@skia//:pathops",
    ],
)

cc_library(
    name = "renderer_skia",
    srcs = ["RendererSkia.cc"],
    hdrs = ["RendererSkia.h"],
    deps = [
        ":renderer_image_io",
        ":renderer_utils",
        ":skia_deps",
        "//donner/svg",
        "//donner/svg/core",
        "//donner/svg/renderer/common",
    ],
)

cc_library(
    name = "renderer",
    visibility = ["//visibility:public"],
    deps = [":renderer_skia"],
)

cc_binary(
    name = "renderer_tool",
    srcs = ["renderer_tool.cc"],
    deps = [
        ":renderer_skia",
        "//donner/svg/parser",
        "@com_google_absl//absl/debugging:failure_signal_handler",
        "@com_google_absl//absl/debugging:symbolize",
    ],
)
