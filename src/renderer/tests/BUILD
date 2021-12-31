load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "renderer_test_utils",
    hdrs = [
        "renderer_test_utils.h",
    ],
    deps = [
        "@com_google_gtest//:gtest",
        "@stb//:image",
    ],
)

cc_test(
    name = "renderer_tests",
    srcs = [
        "renderer_tests.cc",
    ],
    data = [
        "//src/renderer/testdata",
    ],
    deps = [
        ":renderer_test_utils",
        "//src/renderer",
        "//src/svg/xml",
        "@com_google_gtest//:gtest_main",
        "@pixelmatch-cpp17",
    ],
)
