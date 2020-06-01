load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "components",
    srcs = [
        "tree_component.cc",
    ],
    hdrs = [
        "class_component.h",
        "id_component.h",
        "registry.h",
        "transform_component.h",
        "tree_component.h",
    ],
    visibility = ["//src/svg:__subpackages__"],
    deps = [
        "//src/base",
        "@entt",
    ],
)

cc_test(
    name = "components_tests",
    srcs = [
        "tests/tree_component_tests.cc",
    ],
    deps = [
        ":components",
        "@gtest//:gtest_main",
    ],
)
