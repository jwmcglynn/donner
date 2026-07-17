"""Build transition for the Geode editor WebAssembly package."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

def _append_once(values, value):
    result = list(values)
    if value not in result:
        result.append(value)
    return result

def _editor_wasm_geode_transition_impl(settings, _attr):
    return {
        "//build_defs:disable_perf_opt_transition": True,
        "//donner/editor/wasm:enable_wasm": True,
        "//donner/svg/renderer/wasm:enable_wasm": True,
        "//donner/svg/renderer:renderer_backend": "geode",
        "//donner/svg/renderer:text": False,
        "//donner/svg/renderer:text_full": False,
        "//donner/svg/renderer/geode:enable_geode": True,
        "//command_line_option:copt": _append_once(
            settings["//command_line_option:copt"],
            "-pthread",
        ),
        "//command_line_option:cxxopt": _append_once(
            settings["//command_line_option:cxxopt"],
            "-fconstexpr-steps=10000000",
        ),
        "//command_line_option:linkopt": _append_once(
            settings["//command_line_option:linkopt"],
            "-pthread",
        ),
    }

_editor_wasm_geode_transition = transition(
    implementation = _editor_wasm_geode_transition_impl,
    inputs = [
        "//command_line_option:copt",
        "//command_line_option:cxxopt",
        "//command_line_option:linkopt",
    ],
    outputs = [
        "//build_defs:disable_perf_opt_transition",
        "//donner/editor/wasm:enable_wasm",
        "//donner/svg/renderer/wasm:enable_wasm",
        "//donner/svg/renderer:renderer_backend",
        "//donner/svg/renderer:text",
        "//donner/svg/renderer:text_full",
        "//donner/svg/renderer/geode:enable_geode",
        "//command_line_option:copt",
        "//command_line_option:cxxopt",
        "//command_line_option:linkopt",
    ],
)

def _editor_wasm_geode_transitioned_target_impl(ctx):
    dep = ctx.attr.dep
    if type(dep) == "list":
        if len(dep) != 1:
            fail("Geode editor WASM transition produced {} targets, expected 1".format(len(dep)))
        dep = dep[0]

    return [DefaultInfo(files = dep[DefaultInfo].files)]

editor_wasm_geode_transitioned_target = rule(
    implementation = _editor_wasm_geode_transitioned_target_impl,
    attrs = {
        "dep": attr.label(
            cfg = _editor_wasm_geode_transition,
            mandatory = True,
        ),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
)

def _editor_wasm_config_probe_impl(ctx):
    output = ctx.actions.declare_file(ctx.label.name + ".txt")
    values = [
        "copt_pthread={}".format("-pthread" in ctx.fragments.cpp.copts),
        "cxxopt_constexpr={}".format("-fconstexpr-steps=10000000" in ctx.fragments.cpp.cxxopts),
        "disable_perf_opt_transition={}".format(
            ctx.attr._disable_perf_opt_transition[BuildSettingInfo].value,
        ),
        "editor_wasm_enabled={}".format(ctx.attr._editor_wasm_enabled[BuildSettingInfo].value),
        "geode_enabled={}".format(ctx.attr._geode_enabled[BuildSettingInfo].value),
        "linkopt_pthread={}".format("-pthread" in ctx.fragments.cpp.linkopts),
        "renderer_backend={}".format(ctx.attr._renderer_backend[BuildSettingInfo].value),
        "renderer_wasm_enabled={}".format(
            ctx.attr._renderer_wasm_enabled[BuildSettingInfo].value,
        ),
        "text={}".format(ctx.attr._text[BuildSettingInfo].value),
        "text_full={}".format(ctx.attr._text_full[BuildSettingInfo].value),
    ]
    ctx.actions.write(output, "\n".join(values) + "\n")
    return [DefaultInfo(files = depset([output]))]

editor_wasm_config_probe = rule(
    implementation = _editor_wasm_config_probe_impl,
    attrs = {
        "_disable_perf_opt_transition": attr.label(
            default = "//build_defs:disable_perf_opt_transition",
        ),
        "_editor_wasm_enabled": attr.label(default = "//donner/editor/wasm:enable_wasm"),
        "_geode_enabled": attr.label(default = "//donner/svg/renderer/geode:enable_geode"),
        "_renderer_backend": attr.label(default = "//donner/svg/renderer:renderer_backend"),
        "_renderer_wasm_enabled": attr.label(
            default = "//donner/svg/renderer/wasm:enable_wasm",
        ),
        "_text": attr.label(default = "//donner/svg/renderer:text"),
        "_text_full": attr.label(default = "//donner/svg/renderer:text_full"),
    },
    fragments = ["cpp"],
)
