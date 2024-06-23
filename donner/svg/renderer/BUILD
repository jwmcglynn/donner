load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

cc_library(
    name = "renderer_image_io",
    srcs = ["RendererImageIO.cc"],
    hdrs = ["RendererImageIO.h"],
    visibility = ["//visibility:public"],
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
    name = "renderer_skia",
    srcs = ["RendererSkia.cc"],
    hdrs = ["RendererSkia.h"],
    deps = [
        ":renderer_image_io",
        ":renderer_utils",
        "//donner/svg",
        "//donner/svg/core",
        "//donner/svg/renderer/common",
        "@skia//:skia_public",
    ],
)

cc_library(
    name = "renderer_wasm_canvas",
    srcs = ["RendererWasmCanvas.cc"],
    hdrs = ["RendererWasmCanvas.h"],
    # This target won't build successfully on its own because of missing emscripten
    # headers etc. Therefore, we hide it from wildcards.
    tags = ["manual"],
    deps = [
        ":renderer_utils",
        "//donner/svg",
        "//donner/svg/core",
        "//donner/svg/renderer/common",
        "//donner/svg/renderer/wasm_canvas",
    ],
)

cc_library(
    name = "renderer",
    visibility = ["//visibility:public"],
    deps = select({
        "@platforms//cpu:wasm32": [":renderer_wasm_canvas"],
        "@platforms//cpu:wasm64": [":renderer_wasm_canvas"],
        "//conditions:default": [":renderer_skia"],
    }),
)

cc_binary(
    name = "renderer_tool",
    srcs = ["renderer_tool.cc"],
    deps = [
        ":renderer_skia",
        "//donner/svg/xml",
        "@com_google_absl//absl/debugging:failure_signal_handler",
        "@com_google_absl//absl/debugging:symbolize",
    ],
)
