"""Helpers for generating paired native/scalar cc_test targets."""

load("@rules_cc//cc:defs.bzl", "cc_test")
load("//:bazel/simd_transition.bzl", "simd_mode_dep")

def _transition_deps(name, deps, mode):
    transitioned = []
    for i, dep in enumerate(deps):
        transition_name = "%s_%s_dep_%d" % (name, mode, i)
        simd_mode_dep(
            name = transition_name,
            dep = dep,
            simd_mode = mode,
        )
        transitioned.append(":" + transition_name)
    return transitioned

def tiny_skia_dual_mode_cc_test(
        name,
        srcs,
        deps,
        copts = [],
        data = [],
        tags = [],
        **kwargs):
    native_name = name + "_native"
    scalar_name = name + "_scalar"

    cc_test(
        name = native_name,
        srcs = srcs,
        deps = _transition_deps(name, deps, "native"),
        copts = copts,
        data = data,
        tags = tags + ["simd_mode=native"],
        **kwargs
    )

    cc_test(
        name = scalar_name,
        srcs = srcs,
        deps = _transition_deps(name, deps, "scalar"),
        copts = copts,
        data = data,
        tags = tags + ["simd_mode=scalar"],
        **kwargs
    )

    native.test_suite(
        name = name,
        tests = [
            ":" + native_name,
            ":" + scalar_name,
        ],
        tags = tags,
    )
