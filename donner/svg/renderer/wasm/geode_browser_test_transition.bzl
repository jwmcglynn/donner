"""Build the private Geode browser fixture with the browser backend and basic text."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

def _geode_browser_test_transition_impl(_settings, _attr):
    return {
        "//donner/svg/renderer/wasm:enable_wasm": True,
        "//donner/svg/renderer:renderer_backend": "geode",
        "//donner/svg/renderer:text": True,
        "//donner/svg/renderer:text_full": False,
        "//donner/svg/renderer/geode:enable_geode": True,
        "//donner/svg/renderer/geode:browser_backend": True,
        "//command_line_option:compilation_mode": "opt",
    }

_geode_browser_test_transition = transition(
    implementation = _geode_browser_test_transition_impl,
    inputs = [],
    outputs = [
        "//donner/svg/renderer/wasm:enable_wasm",
        "//donner/svg/renderer:renderer_backend",
        "//donner/svg/renderer:text",
        "//donner/svg/renderer:text_full",
        "//donner/svg/renderer/geode:enable_geode",
        "//donner/svg/renderer/geode:browser_backend",
        "//command_line_option:compilation_mode",
    ],
)

def _geode_browser_test_package_impl(ctx):
    package = ctx.attr.package
    if type(package) == "list":
        if len(package) != 1:
            fail("Geode browser transition produced {} packages, expected 1".format(len(package)))
        package = package[0]
    return [DefaultInfo(files = package[DefaultInfo].files)]

geode_browser_test_package = rule(
    implementation = _geode_browser_test_package_impl,
    attrs = {
        "package": attr.label(cfg = _geode_browser_test_transition, mandatory = True),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
)

def _geode_browser_config_probe_impl(ctx):
    output = ctx.actions.declare_file(ctx.label.name + ".json")
    ctx.actions.write(output, json.encode({
        "renderer_backend": ctx.attr._renderer[BuildSettingInfo].value,
        "text": ctx.attr._text[BuildSettingInfo].value,
        "text_full": ctx.attr._full[BuildSettingInfo].value,
        "geode_enabled": ctx.attr._geode[BuildSettingInfo].value,
        "browser_backend": ctx.attr._browser[BuildSettingInfo].value,
        "renderer_wasm_enabled": ctx.attr._wasm[BuildSettingInfo].value,
        "compilation_mode": ctx.var["COMPILATION_MODE"],
    }))
    return [DefaultInfo(files = depset([output]))]

geode_browser_config_probe = rule(
    implementation = _geode_browser_config_probe_impl,
    attrs = {
        "_renderer": attr.label(default = "//donner/svg/renderer:renderer_backend"),
        "_text": attr.label(default = "//donner/svg/renderer:text"),
        "_full": attr.label(default = "//donner/svg/renderer:text_full"),
        "_geode": attr.label(default = "//donner/svg/renderer/geode:enable_geode"),
        "_browser": attr.label(default = "//donner/svg/renderer/geode:browser_backend"),
        "_wasm": attr.label(default = "//donner/svg/renderer/wasm:enable_wasm"),
    },
)
