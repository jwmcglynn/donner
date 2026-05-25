"""Rule for asserting a target does NOT transitively depend on another target.

Companion to `banned_deps.bzl`. Where `banned_deps_test` checks *direct*
(depth-1) dependencies on external libraries, `forbidden_transitive_dep_test`
asserts that `target` has **no path at all** (transitive) to `forbidden` in the
loading-phase target graph.

Uses genquery (`somepath`), evaluated at analysis time — no reentrant Bazel
calls, so it runs inside `bazel test //...` like any other test.

NOTE: genquery evaluates over the *unconfigured* target graph, where every
`select()` branch is present simultaneously. That means this audit is only
meaningful when `target` is a backend-specific library whose own dep list never
mentions `forbidden` (so there is no select branch to hide it). It is the right
tool for `:renderer_geode` (a pure geode library that must never reach
`:renderer_tiny_skia`), but it must NOT be pointed at the dispatcher
`:renderer` — that target legitimately reaches tiny-skia through its
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
