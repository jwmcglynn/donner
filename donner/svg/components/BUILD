load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "transform_component",
    hdrs = [
        "TransformComponent.h",
    ],
    visibility = [
        "//donner/svg:__subpackages__",
    ],
    deps = [
        "//donner/base",
    ],
)

cc_library(
    name = "document_context",
    srcs = [
        "DocumentContext.cc",
    ],
    hdrs = [
        "DocumentContext.h",
        "IdComponent.h",
    ],
    visibility = [
        "//donner/svg:__subpackages__",
    ],
    deps = [
        "//donner/base",
        "//donner/svg/registry",
    ],
)

cc_library(
    name = "components_core",
    srcs = [
        "StylesheetComponent.cc",
        "TreeComponent.cc",
    ],
    hdrs = [
        "AttributesComponent.h",
        "ClassComponent.h",
        "EvaluatedReferenceComponent.h",
        "PathLengthComponent.h",
        "PreserveAspectRatioComponent.h",
        "RenderingBehaviorComponent.h",
        "RenderingInstanceComponent.h",
        "StylesheetComponent.h",
        "TreeComponent.h",
    ],
    visibility = [
        "//donner/css:__pkg__",
        "//donner/svg:__subpackages__",
    ],
    deps = [
        ":document_context",
        ":transform_component",
        "//donner/base/xml",
        "//donner/svg/components/layout:components",
        "//donner/svg/components/shadow:components",
        "//donner/svg/components/style:components",
        "//donner/svg/parser",
        "//donner/svg/properties",
        "//donner/svg/registry",
    ],
)

cc_library(
    name = "components",
    srcs = [
        "RenderingContext.cc",
    ],
    hdrs = [
        "RenderingContext.h",
    ],
    visibility = [
        "//donner/css:__pkg__",
        "//donner/svg:__subpackages__",
    ],
    deps = [
        ":components_core",
        "//donner/svg/components/filter:filter_system",
        "//donner/svg/components/layout:layout_system",
        "//donner/svg/components/paint:paint_system",
        "//donner/svg/components/shadow:components",
        "//donner/svg/components/shadow:shadow_tree_system",
        "//donner/svg/components/shape:shape_system",
    ],
)

cc_test(
    name = "components_tests",
    srcs = [
        "tests/AttributesComponent_tests.cc",
        "tests/TreeComponent_tests.cc",
    ],
    deps = [
        ":components",
        "//donner/base:base_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)
