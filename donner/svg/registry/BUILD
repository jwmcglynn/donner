load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "element_type",
    srcs = [
        "ElementType.cc",
    ],
    hdrs = [
        "ElementType.h",
    ],
    visibility = [
        "//donner/svg:__subpackages__",
    ],
    deps = [
        "//donner/base",
    ],
)

cc_library(
    name = "registry",
    hdrs = [
        "Registry.h",
    ],
    visibility = [
        "//donner/css:__pkg__",
        "//donner/svg:__subpackages__",
    ],
    deps = [
        ":element_type",  # TODO: Remove this dep and update callers to use it directly
        "@entt",
    ],
)
