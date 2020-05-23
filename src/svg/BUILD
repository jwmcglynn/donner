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
    name = "svg",
    deps = [
        ":core",
    ],
    visibility = ["//:__pkg__"],
)
