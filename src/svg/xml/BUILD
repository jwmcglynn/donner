load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

cc_library(
    name = "xml",
    srcs = ["xml_parser.cc"],
    hdrs = ["xml_parser.h"],
    visibility = ["//src/svg:__pkg__"],
    deps = [
        "//src/svg",
        "//src/svg/parser",
        "@rapidxml_ns",
    ],
)

cc_binary(
    name = "xml_tool",
    srcs = ["xml_tool.cc"],
    deps = [":xml"],
)
