load("@rules_cc//cc:defs.bzl", "cc_library")
load("@io_bazel_rules_rust//rust:rust.bzl", "rust_library")

package(default_visibility = ["//visibility:public"])

alias(
    name = "pathfinder_c",
    actual = "@raze__pathfinder_c__0_1_0//:pathfinder_c_static",
)

rust_library(
    name = "pathfinder_c_wrapper",
    srcs = ["lib.rs"],
    crate_type = "staticlib",
    deps = [":pathfinder_c"],
)

cc_library(
    name = "pathfinder_wrapper",
    srcs = [
        ":pathfinder_c_wrapper",
        ":pathfinder_c",
    ],
    hdrs = [":pathfinder_h"],
    include_prefix = ".",
    linkopts = ["-ldl"],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

genrule(
    name = "pathfinder_h",
    srcs = ["@raze__pathfinder_c__0_1_0//:pathfinder_c_build_script_executor"],
    outs = ["pathfinder/pathfinder.h"],
    cmd = "tar -C $(RULEDIR)/pathfinder --strip-components=4 -xf $<",
)
