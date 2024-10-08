load("@rules_cc//cc:defs.bzl", "cc_test")
load("//build_defs:visibility.bzl", "donner_internal_visibility")
load("//build_defs:rules.bzl", "donner_cc_library")

donner_cc_library(
    name = "base",
    hdrs = [
        "Box.h",
        "Length.h",
        "MathUtils.h",
        "OptionalRef.h",
        "RcString.h",
        "RcStringOrRef.h",
        "RelativeLengthMetrics.h",
        "SmallVector.h",
        "StringUtils.h",
        "Transform.h",
        "Utils.h",
        "Vector2.h",
    ],
    visibility = donner_internal_visibility(),
)

donner_cc_library(
    name = "base_test_utils",
    testonly = 1,
    hdrs = [
        "tests/BaseTestUtils.h",
    ],
    visibility = donner_internal_visibility(),
    deps = [
        "@com_google_gtest//:gtest",
    ],
)

cc_test(
    name = "base_tests",
    srcs = [
        "tests/BaseTestUtils_tests.cc",
        "tests/Box_tests.cc",
        "tests/Length_tests.cc",
        "tests/MathUtils_tests.cc",
        "tests/OptionalRef_tests.cc",
        "tests/RcStringOrRef_tests.cc",
        "tests/RcString_tests.cc",
        "tests/SmallVector_tests.cc",
        "tests/StringUtils_tests.cc",
        "tests/Transform_tests.cc",
        "tests/Utils_tests.cc",
        "tests/Vector2_tests.cc",
    ],
    deps = [
        ":base",
        ":base_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)

donner_cc_library(
    name = "base_utils_h_ndebug",
    hdrs = [
        "Utils.h",
    ],
    defines = [
        "NDEBUG",
    ],
)

cc_test(
    name = "base_tests_ndebug",
    srcs = [
        "tests/Utils_tests.cc",
    ],
    defines = [
        "NDEBUG",
    ],
    deps = [
        ":base_utils_h_ndebug",
        "@com_google_gtest//:gtest_main",
    ],
)

cc_test(
    name = "rcstring_tests_with_exceptions",
    srcs = [
        "tests/RcString_tests.cc",
    ],
    copts = [
        "-fexceptions",
    ],
    deps = [
        ":base",
        "@com_google_gtest//:gtest_main",
    ],
)
