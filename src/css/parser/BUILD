load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load("//build:rules.bzl", "donner_cc_fuzzer")

cc_library(
    name = "parser",
    srcs = [
        "color_parser.cc",
        "declaration_list_parser.cc",
        "rule_parser.cc",
        "selector_parser.cc",
        "value_parser.cc",
    ],
    hdrs = [
        "color_parser.h",
        "declaration_list_parser.h",
        "details/common.h",
        "details/subparsers.h",
        "details/tokenizer.h",
        "rule_parser.h",
        "selector_parser.h",
        "value_parser.h",
    ],
    visibility = ["//src/svg:__subpackages__"],
    deps = [
        "//src/base",
        "//src/base/parser",
        "//src/css",
    ],
)

cc_test(
    name = "parser_tests",
    srcs = [
        "tests/color_parser_tests.cc",
        "tests/common_tests.cc",
        "tests/declaration_list_parser_tests.cc",
        "tests/rule_parser_tests.cc",
        "tests/selector_parser_tests.cc",
        "tests/selector_test_utils.h",
        "tests/token_test_utils.h",
        "tests/tokenizer_tests.cc",
        "tests/value_parser_tests.cc",
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
        "@css-parsing-tests//:one_component_value.json",
        "@css-parsing-tests//:one_declaration.json",
        "@css-parsing-tests//:one_rule.json",
        "@css-parsing-tests//:rule_list.json",
        "@css-parsing-tests//:stylesheet.json",
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

donner_cc_fuzzer(
    name = "color_parser_fuzzer",
    srcs = [
        "tests/color_parser_fuzzer.cc",
    ],
    corpus = "tests/color_parser_corpus",
    deps = [
        ":parser",
    ],
)

donner_cc_fuzzer(
    name = "selector_parser_fuzzer",
    srcs = [
        "tests/selector_parser_fuzzer.cc",
    ],
    corpus = "tests/selector_parser_corpus",
    deps = [
        ":parser",
    ],
)
