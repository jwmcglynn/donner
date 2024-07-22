load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "components",
    srcs = [
        "CircleComponent.cc",
        "EllipseComponent.cc",
        "RectComponent.cc",
    ],
    hdrs = [
        "CircleComponent.h",
        "ComputedPathComponent.h",
        "EllipseComponent.h",
        "LineComponent.h",
        "PathComponent.h",
        "PolyComponent.h",
        "RectComponent.h",
    ],
    visibility = [
        "//donner/svg/components:__subpackages__",
    ],
    deps = [
        "//donner/svg/properties",
    ],
)

cc_library(
    name = "shape_system",
    srcs = [
        "ShapeSystem.cc",
    ],
    hdrs = [
        "ShapeSystem.h",
    ],
    visibility = [
        "//donner/svg/components:__subpackages__",
    ],
    deps = [
        ":components",
        "//donner/svg/components:components_core",
        "//donner/svg/components/layout:layout_system",
        "//donner/svg/components/shadow:components",
        "//donner/svg/components/style:style_system",
        "//donner/svg/registry",
    ],
)
