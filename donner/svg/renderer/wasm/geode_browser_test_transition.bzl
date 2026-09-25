"""Build the standalone Geode WASM test page with the browser backend selected."""

def _geode_browser_test_transition_impl(_settings, _attr):
    return {
        "//donner/svg/renderer/wasm:enable_wasm": True,
        "//donner/svg/renderer:renderer_backend": "geode",
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
