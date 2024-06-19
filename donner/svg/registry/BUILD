load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "registry",
    srcs = [
        "Registry.cc",
    ],
    hdrs = [
        "Registry.h",
    ],
    visibility = [
        "//donner/css:__pkg__",
        "//donner/svg:__subpackages__",
    ],
    deps = [
        "//donner/base",
        "@entt",
    ],
)
