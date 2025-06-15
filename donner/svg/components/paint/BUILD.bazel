load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "components",
    srcs = [
        "ClipPathComponent.cc",
        "GradientComponent.cc",
        "LinearGradientComponent.cc",
        "MarkerComponent.cc",
        "MaskComponent.cc",
        "PatternComponent.cc",
        "RadialGradientComponent.cc",
        "StopComponent.cc",
    ],
    hdrs = [
        "ClipPathComponent.h",
        "GradientComponent.h",
        "LinearGradientComponent.h",
        "MarkerComponent.h",
        "MaskComponent.h",
        "PatternComponent.h",
        "RadialGradientComponent.h",
        "StopComponent.h",
    ],
    visibility = [
        "//donner/svg/components:__subpackages__",
    ],
    deps = [
        "//donner/base",
        "//donner/svg/components:components_core",
        "//donner/svg/components/layout:components",
        "//donner/svg/components/shadow:components",
        "//donner/svg/components/shape:components",
        "//donner/svg/properties",
    ],
    alwayslink = 1,
)

cc_library(
    name = "paint_system",
    srcs = [
        "PaintSystem.cc",
    ],
    hdrs = [
        "PaintSystem.h",
    ],
    visibility = [
        "//donner/svg/components:__subpackages__",
    ],
    deps = [
        ":components",
        "//donner/base",
        "//donner/svg/components:components_core",
        "//donner/svg/components/layout:layout_system",
        "//donner/svg/components/shadow:components",
        "//donner/svg/components/style:style_system",
        "//donner/svg/properties",
    ],
)
