load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load("@pip_deps//:requirements.bzl", "requirement")
load("@rules_python//python:defs.bzl", "py_binary")

cc_library(
    name = "core",
    srcs = [
        "path_spline.cc",
    ],
    hdrs = [
        "css_transform.h",
        "gradient.h",
        "path_spline.h",
        "pattern.h",
        "preserve_aspect_ratio.h",
    ],
    visibility = [
        "//src/svg:__subpackages__",
        "//src/svg/renderer:__subpackages__",
    ],
    deps = [
        "//src/base",
        "//src/css:core",
    ],
)

cc_library(
    name = "core_test_utils",
    testonly = 1,
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

py_binary(
    name = "generate_test_pathlength_numpy",
    srcs = ["tests/generate_test_pathlength_numpy.py"],
    deps = [
        requirement("scipy"),
        requirement("numpy"),
    ],
)
