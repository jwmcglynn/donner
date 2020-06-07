load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "harfbuzz",
    srcs = [
        "harfbuzz/src/harfbuzz.cc",
    ],
    hdrs = glob([
        "harfbuzz/src/*.cc",
        "harfbuzz/src/*.hh",
        "harfbuzz/src/*.h",
    ]),
    defines = [
        "HAVE_PTHREAD=1",
    ],
)
