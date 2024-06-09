load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "renderer_test_utils",
    testonly = 1,
    hdrs = [
        "renderer_test_utils.h",
    ],
    visibility = ["//src/svg:__subpackages__"],
    deps = [
        "//src/svg/renderer",
        "//src/svg/tests:xml_test_utils",
        "@com_google_gtest//:gtest",
        "@stb//:image",
    ],
)

cc_library(
    name = "image_comparison_test_fixture",
    testonly = 1,
    srcs = [
        "image_comparison_test_fixture.cc",
    ],
    hdrs = [
        "image_comparison_test_fixture.h",
    ],
    deps = [
        ":renderer_test_utils",
        "//src/svg/renderer",
        "//src/svg/renderer:renderer_image_io",
        "//src/svg/xml",
        "@com_google_gtest//:gtest",
        "@pixelmatch-cpp17",
        "@stb//:image",
    ],
)

cc_test(
    name = "renderer_tests",
    srcs = [
        "renderer_tests.cc",
    ],
    data = [
        "//src/svg/renderer/testdata",
    ],
    deps = [
        ":renderer_test_utils",
        "//src/svg/renderer",
        "//src/svg/renderer:renderer_image_io",
        "//src/svg/tests:xml_test_utils",
        "//src/svg/xml",
        "@com_google_gtest//:gtest_main",
        "@pixelmatch-cpp17",
    ],
)

cc_test(
    name = "resvg_test_suite",
    srcs = [
        "resvg_test_suite.cc",
    ],
    data = [
        "@resvg-test-suite//:png",
        "@resvg-test-suite//:svg",
    ],
    deps = [
        ":image_comparison_test_fixture",
        "@com_google_gtest//:gtest_main",
    ],
)
