cc_library(
    name = "base",
    hdrs = [
        "box.h",
        "math_utils.h",
        "utils.h",
        "vector2.h",
    ],
    visibility = ["//:__subpackages__"],
)

cc_test(
    name = "base_tests",
    srcs = [
        "tests/box_tests.cc",
        "tests/math_utils_tests.cc",
        "tests/vector2_tests.cc",
    ],
    deps = [
        "@gtest//:gtest_main",
        ":base",
    ],
)
