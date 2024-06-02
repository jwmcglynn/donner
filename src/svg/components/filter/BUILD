load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "components",
    srcs = [
        "filter_component.cc",
        "filter_primitive_component.cc",
    ],
    hdrs = [
        "filter_component.h",
        "filter_primitive_component.h",
        "filter_units.h",
    ],
    visibility = [
        "//src/svg/components:__subpackages__",
    ],
    deps = [
        "//src/svg/properties",
    ],
)

cc_library(
    name = "filter_effect",
    srcs = [
        "filter_effect.cc",
    ],
    hdrs = [
        "filter_effect.h",
    ],
    visibility = [
        "//src/svg:__subpackages__",
    ],
    deps = [
        "//src/base",
        "//src/svg/graph",
    ],
)

cc_library(
    name = "filter_system",
    srcs = [
        "filter_system.cc",
    ],
    hdrs = [
        "filter_system.h",
    ],
    visibility = [
        "//src/svg/components:__subpackages__",
    ],
    deps = [
        ":components",
        "//src/svg/components:components_core",
        "//src/svg/registry",
    ],
)
