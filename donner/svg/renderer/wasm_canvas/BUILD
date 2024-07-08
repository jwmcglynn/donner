load("@rules_cc//cc:defs.bzl", "cc_library")
load("//build_defs:visibility.bzl", "donner_internal_visibility")

cc_library(
    name = "wasm_canvas",
    # This target won't build successfully on its own because of missing emscripten
    # headers etc. Therefore, we hide it from wildcards.
    tags = ["manual"],
    visibility = donner_internal_visibility(),
    deps = [
        ":canvas",
    ],
)

cc_library(
    name = "canvas",
    srcs = ["Canvas.cc"],
    hdrs = ["Canvas.h"],
    copts = ["-frtti"],
    # This target won't build successfully on its own because of missing emscripten
    # headers etc. Therefore, we hide it from wildcards.
    tags = ["manual"],
    deps = [
        "//donner/base",
        "//donner/svg/core",
    ],
)
