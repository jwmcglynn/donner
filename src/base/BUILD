load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "base",
    hdrs = [
        "box.h",
        "length.h",
        "math_utils.h",
        "transform.h",
        "utils.h",
        "vector2.h",
    ],
    visibility = ["//:__subpackages__"],
)

cc_library(
    name = "base_test_utils",
    testonly = 1,
    hdrs = [
        "tests/base_test_utils.h",
    ],
    visibility = ["//src:__subpackages__"],
    deps = [
        "@gtest",
    ],
)

cc_test(
    name = "base_tests",
    srcs = [
        "tests/box_tests.cc",
        "tests/math_utils_tests.cc",
        "tests/transform_tests.cc",
        "tests/vector2_tests.cc",
    ],
    deps = [
        ":base",
        ":base_test_utils",
        "@gtest//:gtest_main",
    ],
)
