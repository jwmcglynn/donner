load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "base",
    hdrs = [
        "box.h",
        "length.h",
        "math_utils.h",
        "rc_string.h",
        "relative_length_metrics.h",
        "string_utils.h",
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
        "@com_google_gtest//:gtest",
    ],
)

cc_test(
    name = "base_tests",
    srcs = [
        "tests/base_test_utils_tests.cc",
        "tests/box_tests.cc",
        "tests/length_tests.cc",
        "tests/math_utils_tests.cc",
        "tests/rc_string_tests.cc",
        "tests/string_utils_tests.cc",
        "tests/transform_tests.cc",
        "tests/utils_tests.cc",
        "tests/vector2_tests.cc",
    ],
    deps = [
        ":base",
        ":base_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)

cc_test(
    name = "base_tests_ndebug",
    srcs = [
        "tests/utils_tests.cc",
        "utils.h",
    ],
    defines = [
        "NDEBUG",
    ],
    deps = [
        "@com_google_gtest//:gtest_main",
    ],
)

cc_test(
    name = "rc_string_tests_with_exceptions",
    srcs = [
        "tests/rc_string_tests.cc",
    ],
    copts = [
        "-fexceptions",
    ],
    deps = [
        ":base",
        "@com_google_gtest//:gtest_main",
    ],
)
