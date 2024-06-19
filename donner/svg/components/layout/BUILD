load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "sized_element_component",
    hdrs = [
        "SizedElementComponent.h",
    ],
    visibility = [
        "//donner/svg/components:__subpackages__",
    ],
    deps = [
        "//donner/svg/properties",
    ],
)

cc_library(
    name = "layout_system",
    srcs = [
        "LayoutSystem.cc",
    ],
    hdrs = [
        "LayoutSystem.h",
    ],
    visibility = [
        "//donner/svg/components:__subpackages__",
    ],
    deps = [
        ":sized_element_component",
        "//donner/svg/components:components_core",
        "//donner/svg/components/shadow:components",
        "//donner/svg/components/style:components",
        "//donner/svg/registry",
    ],
)
