load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load("@donner_tools_py_deps//:requirements.bzl", "requirement")
load("@rules_python//python:defs.bzl", "py_binary")

cc_library(
    name = "core_test_utils",
    testonly = 1,
    hdrs = [
        "PathSplineTestUtils.h",
    ],
    visibility = ["//donner/svg:__subpackages__"],
    deps = [
        "//donner/base",
        "//donner/svg/core",
        "@com_google_gtest//:gtest",
    ],
)

cc_test(
    name = "core_tests",
    srcs = [
        "PathSpline_tests.cc",
        "PreserveAspectRatio_tests.cc",
    ],
    deps = [
        ":core_test_utils",
        "//donner/base:base_test_utils",
        "//donner/svg/core",
        "@com_google_gtest//:gtest_main",
    ],
)

py_binary(
    name = "generate_test_pathlength_numpy",
    srcs = ["generate_test_pathlength_numpy.py"],
    deps = [
        requirement("scipy"),
        requirement("numpy"),
    ],
)
