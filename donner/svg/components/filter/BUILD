load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "components",
    srcs = [
        "FilterComponent.cc",
        "FilterPrimitiveComponent.cc",
    ],
    hdrs = [
        "FilterComponent.h",
        "FilterPrimitiveComponent.h",
        "FilterUnits.h",
    ],
    visibility = [
        "//donner/svg/components:__subpackages__",
    ],
    deps = [
        "//donner/svg/properties",
    ],
)

cc_library(
    name = "filter_effect",
    srcs = [
        "FilterEffect.cc",
    ],
    hdrs = [
        "FilterEffect.h",
    ],
    visibility = [
        "//donner/svg:__subpackages__",
    ],
    deps = [
        "//donner/base",
        "//donner/svg/graph",
    ],
)

cc_library(
    name = "filter_system",
    srcs = [
        "FilterSystem.cc",
    ],
    hdrs = [
        "FilterSystem.h",
    ],
    visibility = [
        "//donner/svg/components:__subpackages__",
    ],
    deps = [
        ":components",
        "//donner/base",
        "//donner/svg/components:components_core",
    ],
)
