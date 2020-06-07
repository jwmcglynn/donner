load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "renderer_tool",
    srcs = ["renderer_tool.cc"],
    copts = [
        "-Wno-address-of-temporary",
    ],
    deps = [
        "//src/svg/xml",
        "//third_party/pathfinder_wrapper",
    ],
)
