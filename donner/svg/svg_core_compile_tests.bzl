"""Check that changing the text tier preserves the SVG element compile actions."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("//build_defs:dep_audit.bzl", "configured_dependency_audit_test")

_TEXT = "//donner/svg/renderer:text"
_FULL = "//donner/svg/renderer:text_full"

def _tiers_impl(_settings, _attr):
    return {
        "off": {_TEXT: False, _FULL: False},
        "basic": {_TEXT: True, _FULL: False},
        "full": {_TEXT: True, _FULL: True},
    }

_tiers = transition(implementation = _tiers_impl, inputs = [], outputs = [_TEXT, _FULL])

_Compiles = provider(fields = ["actions"])

def _compiles_impl(target, _ctx):
    return [_Compiles(actions = [a for a in target.actions if a.mnemonic == "CppCompile"])]

_compiles = aspect(implementation = _compiles_impl)

def _normalize_arg(arg):
    # Preserve CPU, compilation mode, exec/target role and the entire filename. Only
    # Bazel's transition hash in an output-directory component varies by construction.
    pieces = arg.split("bazel-out/")
    for i in range(1, len(pieces)):
        directory, slash, rest = pieces[i].partition("/")
        prefix, marker, suffix = directory.rpartition("-ST-")
        if slash and marker and suffix and not suffix.strip("0123456789abcdef"):
            pieces[i] = prefix + slash + rest
    return "bazel-out/".join(pieces)

def _signature(action):
    return {
        "arguments": [_normalize_arg(arg) for arg in action.argv],
        "environment": {k: _normalize_arg(v) for k, v in action.env.items()},
        "inputs": sorted([f.short_path for f in action.inputs.to_list()]),
    }

def _compile_inputs_test_impl(ctx):
    env = unittest.begin(ctx)
    by_tier = {}
    headers = {}
    for tier, target in ctx.split_attr.core.items():
        # Include eligible headers even when Bazel discovers the actually read
        # subset later. Action.inputs alone can omit an unused public header.
        headers[tier] = {f.short_path: True for f in target[CcInfo].compilation_context.headers.to_list()}
        by_tier[tier] = {a.outputs.to_list()[0].short_path: a for a in target[_Compiles].actions}

    reference = by_tier["off"]
    asserts.true(env, bool(reference), "Expected the SVG element implementations to compile C++ sources")
    for tier in ["basic", "full"]:
        if headers["off"] != headers[tier]:
            added = sorted([h for h in headers[tier] if h not in headers["off"]])
            removed = sorted([h for h in headers["off"] if h not in headers[tier]])
            unittest.fail(env, "Compile headers differ between text off and %s: added %s, removed %s" % (tier, added[:5], removed[:5]))
            return unittest.end(env)
        if sorted(reference) != sorted(by_tier[tier]):
            asserts.equals(env, sorted(reference), sorted(by_tier[tier]), "Compile outputs for text " + tier)
            return unittest.end(env)

    # Report the differing action and field without printing the entire compiler input graph.
    for output, action in reference.items():
        expected = _signature(action)
        for tier in ["basic", "full"]:
            for field, actual in _signature(by_tier[tier][output]).items():
                if actual != expected[field]:
                    unittest.fail(env, "%s: compile %s differ between text off and %s" % (output, field, tier))
                    return unittest.end(env)
    return unittest.end(env)

_compile_inputs_test = unittest.make(
    _compile_inputs_test_impl,
    attrs = {
        "core": attr.label(mandatory = True, cfg = _tiers, aspects = [_compiles]),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
    },
)

def svg_core_text_tests(name, core):
    """Checks the element compile actions and implementation dependency boundary in each tier.

    Args:
      name: Prefix for the generated tests.
      core: The C++ target that compiles the common SVG element sources.
    """
    _compile_inputs_test(name = name + "_compile_inputs_test", core = core, size = "small")

    # Stable declaration targets are allowed. The linked system implementations
    # belong to the public facade, even if they happen to expose identical flags.
    for tier in ["none", "basic", "full"]:
        configured_dependency_audit_test(
            name = name + "_" + tier + "_text_dependencies_test",
            target = core,
            text_configuration = tier,
            forbidden = [
                "//donner/base/fonts:woff2_parser",
                "//donner/svg/resources:font_manager",
                "//donner/svg/text:bidi_paragraph",
                "//donner/svg/text:text_backend_full",
                "//donner/svg/text:text_backend_simple",
                "//donner/svg/text:text_engine",
            ],
            forbidden_packages = ["@freetype//", "@harfbuzz//", "@sheenbidi//", "@woff2//"],
            size = "small",
        )
