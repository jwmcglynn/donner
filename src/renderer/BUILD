load("@rules_cc//cc:defs.bzl", "cc_binary")
load("//build:rules.bzl", "copy_pathfinder_resources")

copy_pathfinder_resources(
    name = "copy_resources",
)

cc_binary(
    name = "renderer_tool",
    srcs = ["renderer_tool.cc"],
    copts = [
        "-Wno-address-of-temporary",
    ],
    linkopts = [
        "-lOSMesa",
    ],
    deps = [
        "//src/svg/xml",
        "//third_party/pathfinder_wrapper",
        "@stb//:image_write"
    ],
    data = [
        ":copy_resources",
    ]
)
