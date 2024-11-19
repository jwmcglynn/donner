load("@rules_cc//cc:defs.bzl", "cc_test")
load("//build_defs:rules.bzl", "donner_cc_fuzzer", "donner_cc_library")

donner_cc_library(
    name = "parser",
    srcs = [
        "AnbMicrosyntaxParser.cc",
        "ColorParser.cc",
        "DeclarationListParser.cc",
        "RuleParser.cc",
        "SelectorParser.cc",
        "StylesheetParser.cc",
        "ValueParser.cc",
        "details/Tokenizer.cc",
    ],
    hdrs = [
        "AnbMicrosyntaxParser.h",
        "ColorParser.h",
        "DeclarationListParser.h",
        "RuleParser.h",
        "SelectorParser.h",
        "StylesheetParser.h",
        "ValueParser.h",
        "details/Common.h",
        "details/ComponentValueParser.h",
        "details/Subparsers.h",
        "details/Tokenizer.h",
    ],
    visibility = [
        "//donner/css:__subpackages__",
        "//donner/svg:__subpackages__",
    ],
    deps = [
        "//donner/base",
        "//donner/base/parser",
        "//donner/css:core",
    ],
)

cc_test(
    name = "parser_tests",
    srcs = [
        "tests/AnbMicrosyntaxParser_tests.cc",
        "tests/ColorParser_tests.cc",
        "tests/DeclarationListParser_tests.cc",
        "tests/RuleParser_tests.cc",
        "tests/SelectorParser_tests.cc",
        "tests/StylesheetParser_tests.cc",
        "tests/TokenTestUtils.h",
        "tests/Tokenizer_tests.cc",
        "tests/ValueParser_tests.cc",
    ],
    deps = [
        ":parser",
        "//donner/base:base_test_utils",
        "//donner/css:selector_test_utils",
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
        "@nlohmann_json//:json",
    ],
)

donner_cc_fuzzer(
    name = "declaration_list_parser_fuzzer",
    srcs = [
        "tests/DeclarationListParser_fuzzer.cc",
    ],
    corpus = "tests/declaration_list_parser_corpus",
    deps = [
        ":parser",
    ],
)

donner_cc_fuzzer(
    name = "color_parser_fuzzer",
    srcs = [
        "tests/ColorParser_fuzzer.cc",
    ],
    corpus = "tests/color_parser_corpus",
    deps = [
        ":parser",
    ],
)

donner_cc_fuzzer(
    name = "selector_parser_fuzzer",
    srcs = [
        "tests/SelectorParser_fuzzer.cc",
    ],
    corpus = "tests/selector_parser_corpus",
    deps = [
        ":parser",
    ],
)

donner_cc_fuzzer(
    name = "stylesheet_parser_fuzzer",
    srcs = [
        "tests/StylesheetParser_fuzzer.cc",
    ],
    corpus = "tests/stylesheet_parser_corpus",
    deps = [
        ":parser",
    ],
)

donner_cc_fuzzer(
    name = "anb_microsyntax_parser_fuzzer",
    srcs = [
        "tests/AnbMicrosyntaxParser_fuzzer.cc",
    ],
    corpus = "tests/anb_microsyntax_parser_corpus",
    deps = [
        ":parser",
    ],
)
