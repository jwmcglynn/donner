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
    name = "components_core",
    srcs = [
        "filter_component.cc",
        "filter_primitive_component.cc",
        "pattern_component.cc",
        "stylesheet_component.cc",
        "transform_component.cc",
        "tree_component.cc",
        "viewbox_component.cc",
    ],
    hdrs = [
        "attributes_component.h",
        "class_component.h",
        "evaluated_reference_component.h",
        "filter_component.h",
        "filter_primitive_component.h",
        "path_length_component.h",
        "pattern_component.h",
        "preserve_aspect_ratio_component.h",
        "rendering_behavior_component.h",
        "rendering_instance_component.h",
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
        "//src/svg/components/layout:sized_element_component",
        "//src/svg/components/shadow:components",
        "//src/svg/components/style:components",
        "//src/svg/parser",
        "//src/svg/properties",
        "//src/svg/registry",
        "//src/svg/xml:xml_qualified_name",
    ],
)

cc_library(
    name = "components",
    srcs = [
        "rendering_context.cc",
    ],
    hdrs = [
        "rendering_context.h",
    ],
    visibility = [
        "//src/css:__pkg__",
        "//src/svg:__subpackages__",
    ],
    deps = [
        ":components_core",
        "//src/svg/components/gradient:gradient_system",
        "//src/svg/components/layout:layout_system",
        "//src/svg/components/shadow:components",
        "//src/svg/components/shadow:shadow_tree_system",
        "//src/svg/components/shape:shape_system",
    ],
)

cc_test(
    name = "components_tests",
    srcs = [
        "tests/attributes_component_tests.cc",
        "tests/tree_component_tests.cc",
        "tests/viewbox_component_tests.cc",
    ],
    deps = [
        ":components",
        "//src/base:base_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)
