load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "properties_tests",
    srcs = [
        "paint_server_tests.cc",
        "property_registry_tests.cc",
    ],
    deps = [
        "//src/base:base_test_utils",
        "//src/base/parser:parser_test_utils",
        "//src/svg",
        "//src/svg/properties",
        "@com_google_gtest//:gtest_main",
    ],
)
