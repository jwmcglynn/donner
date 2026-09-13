"""Rule for asserting a target does NOT transitively depend on another target.

Companion to `banned_deps.bzl`. Where `banned_deps_test` checks *direct*
(depth-1) dependencies on external libraries, `forbidden_transitive_dep_test`
asserts that `target` has **no path at all** (transitive) to `forbidden` in the
loading-phase target graph.

Uses genquery (`somepath`), evaluated at analysis time - no reentrant Bazel
calls, so it runs inside `bazel test //...` like any other test.

NOTE: genquery evaluates over the *unconfigured* target graph, where every
`select()` branch is present simultaneously. That means this audit is only
meaningful when `target` is a backend-specific library whose own dep list never
mentions `forbidden` (so there is no select branch to hide it). It is the right
tool for `:renderer_geode` (a pure geode library that must never reach
`:renderer_tiny_skia`), but it must NOT be pointed at the dispatcher
`:renderer` - that target legitimately reaches tiny-skia through its
`//conditions:default` select branch, which the unconfigured graph always
includes.
"""

def forbidden_transitive_dep_test(name, target, forbidden, **kwargs):
    """Test that `target` has no transitive dependency path to `forbidden`.

    Args:
      name: Test target name.
      target: The label whose transitive closure is audited (e.g. the geode
              production renderer //donner/svg/renderer:renderer_geode).
      forbidden: The label that must NOT appear in `target`'s transitive deps
                 (e.g. //donner/svg/renderer:renderer_tiny_skia).
      **kwargs: Forwarded to the underlying sh_test.
    """

    # somepath(target, forbidden) is empty iff there is no dependency path from
    # `target` to `forbidden`. genquery's output file is therefore empty when
    # the invariant holds, and non-empty (lists the path) when it is violated.
    query_expr = "somepath({target}, {forbidden})".format(
        target = target,
        forbidden = forbidden,
    )

    genquery_name = name + "_query"
    native.genquery(
        name = genquery_name,
        expression = query_expr,
        # The scope must contain both endpoints so genquery can load their
        # closures and search for a path between them.
        scope = [target, forbidden],
        testonly = True,
    )

    checker_name = name + "_checker"
    native.genrule(
        name = checker_name,
        srcs = [":" + genquery_name],
        outs = [checker_name + ".sh"],
        cmd = "\n".join([
            "cat > $@ << 'SCRIPT'",
            "#!/bin/bash",
            'input="$$1"',
            'if [ -s "$$input" ]; then',
            '    echo "ERROR: \'{target}\' transitively depends on \'{forbidden}\'."'.format(
                target = target,
                forbidden = forbidden,
            ),
            '    echo "       Production geode must NOT link the tiny-skia backend;"',
            '    echo "       only the test binary links both for in-process parity."',
            "    echo",
            '    echo "Offending dependency path:"',
            '    cat "$$input"',
            "    exit 1",
            "fi",
            'echo "OK: no transitive path from {target} to {forbidden}."'.format(
                target = target,
                forbidden = forbidden,
            ),
            "SCRIPT",
            "chmod +x $@",
        ]),
        testonly = True,
    )

    native.sh_test(
        name = name,
        srcs = [":" + checker_name],
        args = ["$(location :" + genquery_name + ")"],
        data = [":" + genquery_name],
        **kwargs
    )

_ConfiguredDepsInfo = provider(fields = ["labels"])

# Follow binary/library dependencies and the wrappers used to package Wasm.
_CONFIGURED_DEP_ATTRS = ["deps", "implementation_deps", "dep", "cc_target", "wasm_deps"]

def _configured_deps_impl(target, ctx):
    transitive = []
    for name in _CONFIGURED_DEP_ATTRS:
        deps = getattr(ctx.rule.attr, name, [])
        if type(deps) != "list":
            deps = [deps] if deps != None else []
        for dep in deps:
            if _ConfiguredDepsInfo in dep:
                transitive.append(dep[_ConfiguredDepsInfo].labels)
    return [_ConfiguredDepsInfo(labels = depset([target.label], transitive = transitive))]

_configured_deps = aspect(
    implementation = _configured_deps_impl,
    attr_aspects = _CONFIGURED_DEP_ATTRS,
)

def _configured_dependency_audit_impl(ctx):
    labels = ctx.attr.target[_ConfiguredDepsInfo].labels.to_list()
    forbidden = [Label(label) for label in ctx.attr.forbidden]
    missing = [label for label in ctx.attr.required if Label(label) not in labels]
    found = sorted([str(label) for label in labels if label in forbidden])
    for package in ctx.attr.forbidden_packages:
        package_label = Label(package + ":__pkg__")
        found += sorted([
            str(label)
            for label in labels
            if label.workspace_name == package_label.workspace_name and
               (label.package == package_label.package or
                label.package.startswith(package_label.package + "/"))
        ])

    messages = ["Configured dependency audit: " + str(ctx.attr.target.label)]
    messages += ["Forbidden dependency: " + label for label in found]
    messages += ["Missing required dependency: " + label for label in missing]
    if not found and not missing:
        messages.append("PASS: required dependencies present; forbidden dependencies absent")

    output = ctx.actions.declare_file(ctx.label.name + ".sh")
    ctx.actions.write(
        output,
        "#!/bin/sh\ncat <<'AUDIT'\n" + "\n".join(messages) +
        "\nAUDIT\nexit {}\n".format(1 if found or missing else 0),
        is_executable = True,
    )
    return [DefaultInfo(executable = output)]

configured_dependency_audit_test = rule(
    implementation = _configured_dependency_audit_impl,
    test = True,
    attrs = {
        "target": attr.label(mandatory = True, aspects = [_configured_deps]),
        "forbidden": attr.string_list(),
        "forbidden_packages": attr.string_list(),
        "required": attr.string_list(),
    },
    doc = "Audits selected dependency edges after select() and platform transitions, without compiling.",
)
