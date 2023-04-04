load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

cc_library(
    name = "renderer_image_io",
    srcs = ["renderer_image_io.cc"],
    hdrs = ["renderer_image_io.h"],
    visibility = ["//visibility:public"],
    deps = [
        "@stb//:image_write",
    ],
)

cc_library(
    name = "renderer_utils",
    srcs = ["renderer_utils.cc"],
    hdrs = ["renderer_utils.h"],
    visibility = ["//src/svg:__subpackages__"],
    deps = [
        "//src/svg",
    ],
)

cc_library(
    name = "renderer_skia",
    srcs = ["renderer_skia.cc"],
    hdrs = ["renderer_skia.h"],
    deps = [
        ":renderer_image_io",
        ":renderer_utils",
        "//src/svg",
        "//src/svg/core",
        "@skia//:skia_core",
    ],
)

cc_library(
    name = "renderer_wasm_canvas",
    srcs = ["renderer_wasm_canvas.cc"],
    hdrs = ["renderer_wasm_canvas.h"],
    # This target won't build successfully on its own because of missing emscripten
    # headers etc. Therefore, we hide it from wildcards.
    tags = ["manual"],
    deps = [
        ":renderer_utils",
        "//src/svg",
        "//src/svg/core",
        "//src/svg/renderer/wasm_canvas",
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
        "//src/svg/xml",
    ],
)
