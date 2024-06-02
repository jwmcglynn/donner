load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "sized_element_component",
    hdrs = [
        "sized_element_component.h",
    ],
    visibility = [
        "//src/svg/components:__subpackages__",
    ],
    deps = [
        "//src/svg/properties",
    ],
)

cc_library(
    name = "layout_system",
    srcs = [
        "layout_system.cc",
    ],
    hdrs = [
        "layout_system.h",
    ],
    visibility = [
        "//src/svg/components:__subpackages__",
    ],
    deps = [
        ":sized_element_component",
        "//src/svg/components:components_core",
        "//src/svg/components/shadow:components",
        "//src/svg/components/style:components",
        "//src/svg/registry",
    ],
)
