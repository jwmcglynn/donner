load("@rules_cc//cc:defs.bzl", "cc_test")
load("//build_defs:rules.bzl", "donner_cc_library")
load("//build_defs:visibility.bzl", "donner_internal_visibility")

donner_cc_library(
    name = "xml",
    hdrs = [
        # Helper type for an XML attribute name with an optional namespace.
        "XMLQualifiedName.h",
    ],
    visibility = donner_internal_visibility(),
    deps = [
        "//donner/base",
    ],
)

cc_test(
    name = "xml_tests",
    srcs = [
        "tests/XMLQualifiedName_tests.cc",
    ],
    deps = [
        ":xml",
        "@com_google_gtest//:gtest_main",
    ],
)
