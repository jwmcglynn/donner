load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "components",
    hdrs = [
        "computed_style_component.h",
        "style_component.h",
    ],
    visibility = [
        "//src/svg/components:__subpackages__",
    ],
    deps = [
        "//src/svg/properties",
    ],
)

cc_library(
    name = "style_system",
    srcs = [
        "style_system.cc",
    ],
    hdrs = [
        "style_system.h",
    ],
    visibility = [
        "//src/svg/components:__subpackages__",
    ],
    deps = [
        ":components",
        "//src/svg/components:components_core",
        "//src/svg/components/layout:layout_system",
        "//src/svg/components/shadow:components",
        "//src/svg/registry",
    ],
)
