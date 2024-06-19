load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "graph",
    srcs = [
        "Reference.cc",
    ],
    hdrs = [
        "RecursionGuard.h",
        "Reference.h",
    ],
    visibility = [
        "//donner/svg:__subpackages__",
    ],
    deps = [
        "//donner/base",
        "//donner/svg/components:document_context",
    ],
)
