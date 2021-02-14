load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load("//build:rules.bzl", "donner_cc_fuzzer")

cc_library(
    name = "parser",
    srcs = [
        "declaration_list_parser.cc",
    ],
    hdrs = [
        "declaration_list_parser.h",
        "details/common.h",
        "details/subparsers.h",
        "details/tokenizer.h",
    ],
    visibility = ["//src/svg:__subpackages__"],
    deps = [
        "//src/base",
        "//src/base/parser",
        "//src/css",
        "@utfcpp",
    ],
)

cc_test(
    name = "parser_tests",
    srcs = [
        "tests/common_tests.cc",
        "tests/declaration_list_parser_tests.cc",
        "tests/token_test_utils.cc",
        "tests/token_test_utils.h",
        "tests/tokenizer_tests.cc",
    ],
    deps = [
        ":parser",
        "//src/base/parser:parser_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)

cc_test(
    name = "css_parsing_tests",
    srcs = [
        "tests/css_parsing_tests.cc",
    ],
    data = [
        "@css-parsing-tests//:component_value_list.json",
        "@css-parsing-tests//:declaration_list.json",
    ],
    deps = [
        ":parser",
        "@com_google_gtest//:gtest_main",
        "@nlohmann_json",
    ],
)

donner_cc_fuzzer(
    name = "declaration_list_parser_fuzzer",
    srcs = [
        "tests/declaration_list_parser_fuzzer.cc",
    ],
    corpus = "tests/declaration_list_parser_corpus",
    deps = [
        ":parser",
    ],
)
