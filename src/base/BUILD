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
    name = "base_test",
    srcs = [
        "box_tests.cc",
        "math_utils_tests.cc",
        "vector2_tests.cc",
    ],
    deps = [
        "@gtest//:gtest_main",
        ":base",
    ],
)
