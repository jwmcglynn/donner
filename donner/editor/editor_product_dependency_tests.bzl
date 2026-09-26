"""Configured product audits: full-text editors can coexist with smaller core SVG builds."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("//build_defs:dep_audit.bzl", "configured_dependency_audit_test")
load(":editor_product_config.bzl", "editor_full_text_guard")

_EDITOR_ROOTS = {
    # The macOS app-bundle rule wraps the transitioned binary; the configured
    # dependency audit follows the binary provider's actual C++ graph.
    "native": "//donner/editor:editor_binary",
    "render_repl": "//donner/editor/app:render_repl",
    "replay": "//donner/editor/tests:editor_rnr_gl_replay",
    "mcp": "//tools/mcp-servers/editor-control:editor_control_mcp_server",
    "showcase": "//donner/editor/tools:generate_showcase_asset",
    "shell_tests": "//donner/editor/tests:editor_shell_tests",
    "render_session_tests": "//donner/editor/app/tests:render_session_tests",
    "mcp_tests": "//tools/mcp-servers/editor-control:editor_control_session_tests",
    "wasm": "//donner/editor/wasm:serve_http",
}

# Resolve in this repository before Skylib creates its analysis-test transition.
_TEXT_SETTING = str(Label("//donner/svg/renderer:text"))
_FULL_TEXT_SETTING = str(Label("//donner/svg/renderer:text_full"))

_FULL_TEXT_DEPS = [
    "//donner/base/fonts:woff2_parser",
    "//donner/svg/text:text_backend_full",
    "@woff2//:woff2_decode",
]

def _guard_rejects_incomplete_text_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.expect_failure(env, "Editor implementation requires --//donner/svg/renderer:text=true and --//donner/svg/renderer:text_full=true")
    return analysistest.end(env)

_guard_rejects_basic_test = analysistest.make(
    _guard_rejects_incomplete_text_impl,
    expect_failure = True,
    config_settings = {
        _TEXT_SETTING: True,
        _FULL_TEXT_SETTING: False,
    },
)

_guard_rejects_no_text_test = analysistest.make(
    _guard_rejects_incomplete_text_impl,
    expect_failure = True,
    config_settings = {
        _TEXT_SETTING: False,
        _FULL_TEXT_SETTING: False,
    },
)

_guard_rejects_text_full_without_text_test = analysistest.make(
    _guard_rejects_incomplete_text_impl,
    expect_failure = True,
    config_settings = {
        _TEXT_SETTING: False,
        _FULL_TEXT_SETTING: True,
    },
)

def _guard_accepts_full_text_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.true(env, CcInfo in analysistest.target_under_test(env))
    return analysistest.end(env)

_guard_accepts_full_text_test = analysistest.make(
    _guard_accepts_full_text_impl,
    config_settings = {
        _TEXT_SETTING: True,
        _FULL_TEXT_SETTING: True,
    },
)

def editor_product_dependency_tests():
    """Audit actual products and representative integration tests under smaller ambient tiers."""
    editor_full_text_guard(
        name = "_editor_full_text_guard_probe",
        testonly = True,
        tags = ["manual"],
    )
    guard_tests = {
        "editor_guard_rejects_basic_test": _guard_rejects_basic_test,
        "editor_guard_rejects_no_text_test": _guard_rejects_no_text_test,
        "editor_guard_rejects_text_full_without_text_test": _guard_rejects_text_full_without_text_test,
        "editor_guard_accepts_full_text_test": _guard_accepts_full_text_test,
    }
    tests = []
    for name, guard_test in guard_tests.items():
        guard_test(name = name, target_under_test = ":_editor_full_text_guard_probe")
        tests.append(":" + name)
    for tier in ["basic", "none"]:
        for bypass in [False, True]:
            suffix = tier + ("_backend_transition_disabled" if bypass else "")
            for product, target in _EDITOR_ROOTS.items():
                name = "editor_" + product + "_" + suffix + "_dependency_test"
                configured_dependency_audit_test(
                    name = name,
                    target = target,
                    text_configuration = tier,
                    disable_backend_transitions = bypass,
                    required = _FULL_TEXT_DEPS + ["//donner/editor:requires_text_true_and_text_full_true"],
                    size = "small",
                )
                tests.append(":" + name)
        name = "core_" + tier + "_excludes_full_text_dependency_test"
        configured_dependency_audit_test(
            name = name,
            target = "//donner/svg",
            text_configuration = tier,
            forbidden = _FULL_TEXT_DEPS + [
                "//third_party:freetype",
                "@harfbuzz//:harfbuzz",
            ],
            size = "small",
        )
        tests.append(":" + name)
    native.test_suite(name = "editor_product_dependency_tests", tests = tests)
