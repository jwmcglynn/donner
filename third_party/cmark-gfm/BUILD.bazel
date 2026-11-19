load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "generated_files",
    hdrs = [
        "cmark-gfm_export.h",
        "config.h",
    ],
    copts = ["-I."],
    includes = ["."],
    visibility = ["//visibility:public"],
)
