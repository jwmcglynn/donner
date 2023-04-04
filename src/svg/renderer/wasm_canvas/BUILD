load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "wasm_canvas",
    # This target won't build successfully on its own because of missing emscripten
    # headers etc. Therefore, we hide it from wildcards.
    tags = ["manual"],
    visibility = ["//visibility:public"],
    deps = [
        ":canvas",
    ],
)

cc_library(
    name = "canvas",
    srcs = ["canvas.cc"],
    hdrs = ["canvas.h"],
    # This target won't build successfully on its own because of missing emscripten
    # headers etc. Therefore, we hide it from wildcards.
    tags = ["manual"],
    deps = [
        "//src/base",
        "//src/svg/core",
    ],
)
