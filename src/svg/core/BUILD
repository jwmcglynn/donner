load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "core",
    srcs = [
        "path_spline.cc",
    ],
    hdrs = [
        "path_spline.h",
        "preserve_aspect_ratio.h",
    ],
    visibility = [
        "//src/svg:__subpackages__",
        "//src/svg/renderer:__subpackages__",
    ],
    deps = [
        "//src/base",
    ],
)

cc_library(
    name = "core_test_utils",
    testonly = 1,
    srcs = [
        "tests/path_spline_test_utils.cc",
    ],
    hdrs = [
        "tests/path_spline_test_utils.h",
    ],
    visibility = ["//src/svg:__subpackages__"],
    deps = [
        ":core",
        "//src/base",
        "@com_google_gtest//:gtest",
    ],
)

cc_test(
    name = "core_tests",
    srcs = [
        "tests/path_spline_tests.cc",
    ],
    deps = [
        ":core",
        ":core_test_utils",
        "//src/base:base_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)
