load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "css",
    srcs = [
        "color.cc",
        "declaration.cc",
    ],
    hdrs = [
        "color.h",
        "declaration.h",
        "rule.h",
        "selector.h",
        "token.h",
    ],
    visibility = [
        "//src/css:__subpackages__",
        "//src/svg:__subpackages__",
    ],
    deps = [
        "//src/base",
        "@frozen",
    ],
)
