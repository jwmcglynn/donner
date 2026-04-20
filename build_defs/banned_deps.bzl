"""Rule for verifying that no targets directly depend on banned external deps."""

def banned_deps_test(name, banned, allowed, scope, **kwargs):
    """
    Test that no target in `scope` directly depends on any label in `banned`,
    except for the targets listed in `allowed`.

    Uses genquery (evaluated at analysis time) — no reentrant Bazel calls.

    Args:
      name: Test target name.
      banned: List of labels that must not appear as direct deps
              (e.g. ["@zlib", "@freetype"]).
      allowed: List of labels that ARE permitted to depend on the banned deps
               (e.g. ["//third_party:zlib", "//third_party:freetype"]).
      scope: List of concrete top-level targets whose transitive closure
             defines the query universe.  genquery requires concrete labels,
             not target patterns like "//donner/...".
      **kwargs: Forwarded to the underlying sh_test.
    """

    universe = " + ".join([str(s) for s in scope])

    # Build a query expression that finds every target in the transitive
    # closure of `scope` which directly depends (depth 1) on any banned
    # label, minus the allowed set.
    parts = []
    for dep in banned:
        parts.append("rdeps({universe}, {dep}, 1)".format(
            universe = universe,
            dep = dep,
        ))

    # Union all rdeps results, then subtract the banned labels themselves
    # and the allowed wrapper targets.
    subtract = list(banned) + list(allowed)
    # Wrap with filter to exclude external repository targets (e.g.
    # @harfbuzz) which legitimately depend on @zlib/@freetype internally.
    # We only care about first-party (//...) targets.
    query_expr = "filter(\"^//\", ({union}) - set({subtract}))".format(
        union = " + ".join(parts),
        subtract = " ".join(subtract),
    )

    genquery_name = name + "_query"
    native.genquery(
        name = genquery_name,
        expression = query_expr,
        scope = scope,
        testonly = True,
    )

    # The genquery output is empty when there are no violations.
    # The test script checks for a non-empty file.
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
            '    echo "ERROR: The following targets directly depend on banned external deps."',
            '    echo "       Use the //third_party wrapper targets instead."',
            "    echo",
            '    cat "$$input"',
            "    exit 1",
            "fi",
            'echo "OK: No banned direct dependencies found."',
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
