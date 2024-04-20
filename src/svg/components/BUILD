load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "transform_component",
    hdrs = [
        "transform_component.h",
    ],
    visibility = [
        "//src/svg:__subpackages__",
    ],
    deps = [
        "//src/base",
    ],
)

cc_library(
    name = "document_context",
    srcs = [
        "document_context.cc",
    ],
    hdrs = [
        "document_context.h",
        "id_component.h",
    ],
    visibility = [
        "//src/svg:__subpackages__",
    ],
    deps = [
        "//src/base",
        "//src/svg/registry",
    ],
)

cc_library(
    name = "components",
    srcs = [
        "circle_component.cc",
        "computed_style_component.cc",
        "ellipse_component.cc",
        "gradient_component.cc",
        "line_component.cc",
        "linear_gradient_component.cc",
        "path_component.cc",
        "pattern_component.cc",
        "poly_component.cc",
        "radial_gradient_component.cc",
        "rect_component.cc",
        "rendering_context.cc",
        "sized_element_component.cc",
        "stop_component.cc",
        "stylesheet_component.cc",
        "transform_component.cc",
        "tree_component.cc",
        "viewbox_component.cc",
    ],
    hdrs = [
        "attributes_component.h",
        "circle_component.h",
        "class_component.h",
        "computed_path_component.h",
        "computed_shadow_tree_component.h",
        "computed_style_component.h",
        "ellipse_component.h",
        "evaluated_reference_component.h",
        "gradient_component.h",
        "line_component.h",
        "linear_gradient_component.h",
        "offscreen_shadow_tree_component.h",
        "path_component.h",
        "path_length_component.h",
        "pattern_component.h",
        "poly_component.h",
        "preserve_aspect_ratio_component.h",
        "radial_gradient_component.h",
        "rect_component.h",
        "rendering_behavior_component.h",
        "rendering_context.h",
        "rendering_instance_component.h",
        "shadow_entity_component.h",
        "shadow_tree_component.h",
        "sized_element_component.h",
        "stop_component.h",
        "style_component.h",
        "stylesheet_component.h",
        "tree_component.h",
        "viewbox_component.h",
    ],
    visibility = [
        "//src/css:__pkg__",
        "//src/svg:__subpackages__",
    ],
    deps = [
        ":document_context",
        ":transform_component",
        "//src/svg/parser",
        "//src/svg/properties",
        "//src/svg/registry",
        "//src/svg/xml:xml_attribute",
    ],
)

cc_test(
    name = "components_tests",
    srcs = [
        "tests/tree_component_tests.cc",
        "tests/viewbox_component_tests.cc",
    ],
    deps = [
        ":components",
        "//src/base:base_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)
