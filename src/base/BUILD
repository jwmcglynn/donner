load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "base",
    hdrs = [
        "box.h",
        "length.h",
        "math_utils.h",
        "rc_string.h",
        "string_utils.h",
        "transform.h",
        "utils.h",
        "vector2.h",
    ],
    visibility = ["//:__subpackages__"],
    deps = [
        "@range-v3",
    ],
)

cc_library(
    name = "base_test_utils",
    testonly = 1,
    hdrs = [
        "tests/base_test_utils.h",
    ],
    visibility = ["//src:__subpackages__"],
    deps = [
        "@com_google_gtest//:gtest",
    ],
)

cc_test(
    name = "base_tests",
    srcs = [
        "tests/base_test_utils_tests.cc",
        "tests/box_tests.cc",
        "tests/math_utils_tests.cc",
        "tests/rc_string_tests.cc",
        "tests/string_utils_tests.cc",
        "tests/transform_tests.cc",
        "tests/vector2_tests.cc",
    ],
    deps = [
        ":base",
        ":base_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)
