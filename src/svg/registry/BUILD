load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "registry",
    srcs = [
        "registry.cc",
    ],
    hdrs = [
        "registry.h",
    ],
    visibility = [
        "//src/css:__pkg__",
        "//src/svg:__subpackages__",
    ],
    deps = [
        "//src/base",
        "@entt",
    ],
)
