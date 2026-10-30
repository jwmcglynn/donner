"""A native product built in the configuration it ships in, so its linked size is measurable."""

load("//build_defs:rules.bzl", "DonnerTransitionedBinaryInfo")

def _shipped_configuration_impl(settings, _attr):
    # Use size optimization, Geode, basic text and no Tracy profiler client. Products apply their
    # own transitions on top, as the editor does for full text.
    return {
        "//build_defs:enable_tracy": False,
        "//command_line_option:compilation_mode": "opt",
        "//command_line_option:copt": settings["//command_line_option:copt"] + ["-Os"],
        "//donner/svg/renderer/geode:enable_geode": True,
        "//donner/svg/renderer:renderer_backend": "geode",
        "//donner/svg/renderer:text": True,
        "//donner/svg/renderer:text_full": False,
    }

_shipped_configuration = transition(
    implementation = _shipped_configuration_impl,
    inputs = ["//command_line_option:copt"],
    outputs = [
        "//build_defs:enable_tracy",
        "//command_line_option:compilation_mode",
        "//command_line_option:copt",
        "//donner/svg/renderer/geode:enable_geode",
        "//donner/svg/renderer:renderer_backend",
        "//donner/svg/renderer:text",
        "//donner/svg/renderer:text_full",
    ],
)

def _shipped_product_impl(ctx):
    product = ctx.attr.product
    if type(product) == "list":
        if len(product) != 1:
            fail("the shipped configuration produced {} targets, expected 1".format(len(product)))
        product = product[0]

    # A product-configured wrapper (the editor) is a launcher script; measure the linked binary
    # behind it.
    if DonnerTransitionedBinaryInfo in product:
        executable = product[DonnerTransitionedBinaryInfo].binary
    else:
        executable = product[DefaultInfo].files_to_run.executable
    if executable == None:
        fail("{} is not an executable".format(ctx.attr.product))
    return [DefaultInfo(
        files = depset([executable]),
        runfiles = ctx.runfiles(files = [executable]),
    )]

shipped_product = rule(
    implementation = _shipped_product_impl,
    doc = "Exposes `product`'s linked executable built optimized for size with Geode and without Tracy.",
    attrs = {
        "product": attr.label(
            cfg = _shipped_configuration,
            executable = True,
            mandatory = True,
        ),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
)
