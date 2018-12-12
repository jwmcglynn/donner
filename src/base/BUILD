cc_library(
    name = "base",
    hdrs = [
        "math_utils.h",
        "utils.h",
    ],
    visibility = ["//:__pkg__"],
)

cc_test(
    name = "base_test",
    srcs = [
        "math_utils_tests.cc",
    ],
    deps = [
        "@gtest//:gtest_main",
        ":base",
    ],
)
