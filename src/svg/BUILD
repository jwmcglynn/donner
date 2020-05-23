cc_library(
    name = "core",
    hdrs = [
        "core/path_spline.h",
    ],
    srcs = [
        "core/path_spline.cc",
    ],
    deps = [
        "//src/base",
    ]
)

cc_test(
    name = "core_tests",
    srcs = [
        "core/tests/path_spline_tests.cc",
    ],
    deps = [
        "@gtest//:gtest_main",
        ":core",
    ],
)

cc_library(
    name = "parser",
    hdrs = [
        "parser/parse_error.h",
        "parser/parse_result.h",
        "parser/path_parser.h",
    ],
    srcs = [
        "parser/parse_error.cc",
        "parser/path_parser.cc",
    ],
    deps = [
        ":core",
        "//src/base",
    ]
)

cc_test(
    name = "parser_tests",
    srcs = [
        "parser/tests/parse_result_test_utils.h",
        "parser/tests/path_parser_tests.cc",
    ],
    deps = [
        "@gtest//:gtest_main",
        ":parser",
    ],
)

cc_library(
    name = "svg",
    deps = [
        ":core",
    ],
    visibility = ["//:__pkg__"],
)
