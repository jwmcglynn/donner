load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "components",
    hdrs = [
        "SizedElementComponent.h",
        "TransformComponent.h",
        "ViewboxComponent.h",
    ],
    visibility = [
        "//donner/svg:__subpackages__",
    ],
    deps = [
        "//donner/svg/properties:property",
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
        "//donner/base",
        "//donner/svg/components:components_core",
        "//donner/svg/components/paint:components",
        "//donner/svg/components/resources:resource_manager_context",
        "//donner/svg/components/shadow:components",
        "//donner/svg/components/style:components",
        "//donner/svg/components/style:style_system",
    ],
)

cc_test(
    name = "layout_system_tests",
    srcs = [
        "tests/LayoutSystem_tests.cc",
    ],
    deps = [
        ":layout_system",
        "//donner/base:base_test_utils",
        "//donner/base/parser:parser_test_utils",
        "//donner/svg/xml",
        "@com_google_gtest//:gtest_main",
    ],
)
