load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")
load("//build:rules.bzl", "copy_pathfinder_resources")

cc_library(
    name = "render_context_osmesa",
    srcs = ["render_context_osmesa.cc"],
    hdrs = ["render_context_osmesa.h"],
    linkopts = ["-lOSMesa"],
    deps = [
        "@stb//:image_write",
    ],
)

copy_pathfinder_resources(
    name = "copy_resources",
)

cc_binary(
    name = "renderer_tool",
    srcs = ["renderer_tool.cc"],
    copts = [
        "-Wno-address-of-temporary",
    ],
    data = [
        ":copy_resources",
    ],
    deps = [
        ":render_context_osmesa",
        "//src/svg/xml",
        "//third_party/pathfinder_wrapper",
    ],
)
