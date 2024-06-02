load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "components",
    srcs = [
        "circle_component.cc",
        "ellipse_component.cc",
        "rect_component.cc",
    ],
    hdrs = [
        "circle_component.h",
        "computed_path_component.h",
        "ellipse_component.h",
        "line_component.h",
        "path_component.h",
        "poly_component.h",
        "rect_component.h",
    ],
    visibility = [
        "//src/svg/components:__subpackages__",
    ],
    deps = [
        "//src/svg/properties",
    ],
)

cc_library(
    name = "shape_system",
    srcs = [
        "shape_system.cc",
    ],
    hdrs = [
        "shape_system.h",
    ],
    visibility = [
        "//src/svg/components:__subpackages__",
    ],
    deps = [
        ":components",
        "//src/svg/components:components_core",
        "//src/svg/components/shadow:components",
        "//src/svg/components/style:style_system",
        "//src/svg/registry",
    ],
)
