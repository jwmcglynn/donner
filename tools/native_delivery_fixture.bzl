"""Test-only ELF fixture data with source lines preserved by its build configuration."""

def _keep_fixture_debug_impl(_settings, _attr):
    return {"//command_line_option:strip": "never"}

_keep_fixture_debug = transition(
    implementation = _keep_fixture_debug_impl,
    inputs = [],
    outputs = ["//command_line_option:strip"],
)

def _native_delivery_fixture_impl(ctx):
    if not ctx.attr.testonly:
        fail("native delivery fixture data must be test-only")
    binary = ctx.attr.binary
    if type(binary) == "list":
        if len(binary) != 1:
            fail("fixture configuration must produce one binary")
        binary = binary[0]
    image = binary[DefaultInfo].files_to_run.executable
    fixture = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.symlink(output = fixture, target_file = image)
    return [DefaultInfo(
        files = depset([fixture]),
        runfiles = ctx.runfiles(files = [fixture, image]),
    )]

native_delivery_fixture = rule(
    implementation = _native_delivery_fixture_impl,
    doc = "Expose an unstripped test image under a stable data filename, without executing it.",
    attrs = {
        "binary": attr.label(cfg = _keep_fixture_debug, executable = True, mandatory = True),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
)
