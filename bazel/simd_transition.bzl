"""SIMD mode transitions and helper rules."""

def _simd_mode_transition_impl(_settings, attr):
    return {"//bazel/config:simd_mode": attr.simd_mode}

simd_mode_transition = transition(
    implementation = _simd_mode_transition_impl,
    inputs = [],
    outputs = ["//bazel/config:simd_mode"],
)

def _simd_mode_dep_impl(ctx):
    dep = ctx.attr.dep
    if type(dep) == "list":
        dep = dep[0]

    return [
        dep[CcInfo],
        dep[DefaultInfo],
    ]

_simd_mode_dep = rule(
    implementation = _simd_mode_dep_impl,
    attrs = {
        "dep": attr.label(
            mandatory = True,
            cfg = simd_mode_transition,
            providers = [CcInfo],
        ),
        "simd_mode": attr.string(
            mandatory = True,
            values = ["native", "scalar"],
        ),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
)

def simd_mode_dep(name, dep, simd_mode, visibility = None):
    _simd_mode_dep(
        name = name,
        dep = dep,
        simd_mode = simd_mode,
        visibility = visibility,
    )
