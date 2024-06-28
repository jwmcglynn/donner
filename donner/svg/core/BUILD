load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load("@pip_deps//:requirements.bzl", "requirement")
load("@rules_python//python:defs.bzl", "py_binary")

cc_library(
    name = "core",
    srcs = [
        "PathSpline.cc",
        "PreserveAspectRatio.cc",
    ],
    hdrs = [
        "ClipPathUnits.h",
        "ClipRule.h",
        "CssTransform.h",
        "Gradient.h",
        "PathSpline.h",
        "Pattern.h",
        "PreserveAspectRatio.h",
    ],
    visibility = [
        "//donner/svg:__subpackages__",
        "//donner/svg/renderer:__subpackages__",
    ],
    deps = [
        "//donner/base",
        "//donner/css:core",
    ],
)

cc_library(
    name = "core_test_utils",
    testonly = 1,
    hdrs = [
        "tests/PathSplineTestUtils.h",
    ],
    visibility = ["//donner/svg:__subpackages__"],
    deps = [
        ":core",
        "//donner/base",
        "@com_google_gtest//:gtest",
    ],
)

cc_test(
    name = "core_tests",
    srcs = [
        "tests/PathSpline_tests.cc",
        "tests/PreserveAspectRatio_tests.cc",
    ],
    deps = [
        ":core",
        ":core_test_utils",
        "//donner/base:base_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)

py_binary(
    name = "generate_test_pathlength_numpy",
    srcs = ["tests/generate_test_pathlength_numpy.py"],
    deps = [
        requirement("scipy"),
        requirement("numpy"),
    ],
)
