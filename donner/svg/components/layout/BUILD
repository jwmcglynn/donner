load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "components",
    hdrs = [
        "SizedElementComponent.h",
        "ViewboxComponent.h",
    ],
    visibility = [
        "//donner/svg:__subpackages__",
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
        ":components",
        "//donner/svg/components:components_core",
        "//donner/svg/components/shadow:components",
        "//donner/svg/components/style:components",
        "//donner/svg/components/style:style_system",
        "//donner/svg/registry",
    ],
)
