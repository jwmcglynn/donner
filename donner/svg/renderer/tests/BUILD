load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "renderer_test_utils",
    testonly = 1,
    srcs = [
        "RendererTestUtils.cc",
    ],
    hdrs = [
        "RendererTestUtils.h",
    ],
    visibility = ["//donner/svg:__subpackages__"],
    deps = [
        "//donner/svg/renderer",
        "//donner/svg/tests:xml_test_utils",
        "@com_google_gtest//:gtest",
        "@stb//:image",
    ],
)

cc_library(
    name = "image_comparison_test_fixture",
    testonly = 1,
    srcs = [
        "ImageComparisonTestFixture.cc",
    ],
    hdrs = [
        "ImageComparisonTestFixture.h",
    ],
    deps = [
        ":renderer_test_utils",
        "//donner/svg/renderer",
        "//donner/svg/renderer:renderer_image_io",
        "//donner/svg/xml",
        "@com_google_gtest//:gtest",
        "@pixelmatch-cpp17",
        "@stb//:image",
    ],
)

cc_test(
    name = "renderer_tests",
    srcs = [
        "Renderer_tests.cc",
    ],
    data = [
        "//donner/svg/renderer/testdata",
    ],
    deps = [
        ":renderer_test_utils",
        "//donner/svg/renderer",
        "//donner/svg/renderer:renderer_image_io",
        "//donner/svg/tests:xml_test_utils",
        "//donner/svg/xml",
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
