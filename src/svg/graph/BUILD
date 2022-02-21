load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "graph",
    srcs = [
        "reference.cc",
    ],
    hdrs = [
        "reference.h",
    ],
    visibility = [
        "//src/svg:__subpackages__",
    ],
    deps = [
        "//src/base",
        "//src/svg/components:document_context",
    ],
)
