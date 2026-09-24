"""Builds a target with WebAssembly threads enabled, as the editor's module is."""

def _append_once(values, value):
    result = list(values)
    if value not in result:
        result.append(value)
    return result

def _pthreads_transition_impl(settings, _attr):
    return {
        "//command_line_option:copt": _append_once(settings["//command_line_option:copt"], "-pthread"),
        "//command_line_option:linkopt": _append_once(
            settings["//command_line_option:linkopt"],
            "-pthread",
        ),
    }

_pthreads_transition = transition(
    implementation = _pthreads_transition_impl,
    inputs = [
        "//command_line_option:copt",
        "//command_line_option:linkopt",
    ],
    outputs = [
        "//command_line_option:copt",
        "//command_line_option:linkopt",
    ],
)

def _pthreads_transitioned_target_impl(ctx):
    dep = ctx.attr.dep
    if type(dep) == "list":
        if len(dep) != 1:
            fail("the threads transition produced {} targets, expected 1".format(len(dep)))
        dep = dep[0]
    return [DefaultInfo(files = dep[DefaultInfo].files)]

pthreads_transitioned_target = rule(
    implementation = _pthreads_transitioned_target_impl,
    doc = "Builds `dep`, and everything it depends on, with `-pthread` compile and link options.",
    attrs = {
        "dep": attr.label(
            cfg = _pthreads_transition,
            mandatory = True,
        ),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
)
