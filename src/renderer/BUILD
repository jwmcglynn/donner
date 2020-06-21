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

cc_library(
    name = "renderer_pathfinder",
    srcs = ["renderer_pathfinder.cc"],
    hdrs = ["renderer_pathfinder.h"],
    deps = [
        "//src/svg",
        "//src/svg/core",
        "//third_party/pathfinder_wrapper",
    ],
)

copy_pathfinder_resources(
    name = "copy_resources",
)

cc_binary(
    name = "renderer_tool",
    srcs = ["renderer_tool.cc"],
    data = [
        ":copy_resources",
    ],
    deps = [
        ":render_context_osmesa",
        ":renderer_pathfinder",
        "//src/svg/xml",
    ],
)
