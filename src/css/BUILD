load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "css",
    srcs = [
        "declaration.cc",
    ],
    hdrs = [
        "declaration.h",
        "stylesheet.h",
        "token.h",
    ],
    visibility = ["//src/css:__subpackages__"],
    deps = [
        "//src/base",
    ],
)
