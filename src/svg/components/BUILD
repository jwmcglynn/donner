load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "components",
    srcs = [
        "computed_path_component.cc",
        "path_component.cc",
        "tree_component.cc",
        "viewbox_component.cc",
    ],
    hdrs = [
        "class_component.h",
        "computed_path_component.h",
        "document_context.h",
        "id_component.h",
        "path_component.h",
        "rect_component.h",
        "registry.h",
        "sized_element_component.h",
        "style_component.h",
        "transform_component.h",
        "tree_component.h",
        "viewbox_component.h",
    ],
    visibility = [
        "//src/css:__pkg__",
        "//src/svg:__subpackages__",
    ],
    deps = [
        "//src/base",
        "//src/svg/parser",
        "//src/svg/properties",
        "@entt",
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
