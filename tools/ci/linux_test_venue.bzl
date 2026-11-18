"""Admit local display actions in the mixed Linux test invocation."""

_DISPLAY_TESTS = [
    Label("//donner/editor/tests:editor_window_vulkan_default_tests"),
    Label("//donner/editor/tests:editor_window_vulkan_surface_tests"),
]

def _linux_test_venue_impl(target, _ctx):
    for action in target.actions:
        if action.mnemonic != "TestRunner":
            continue

        # Bazel 8 exposes the final action requirements (including platform and command-line
        # modifiers) with --experimental_google_legacy_api. Missing metadata must fail admission.
        requirements = action.execution_info
        local_only = any([key in requirements for key in ["local", "no-remote", "no-remote-exec"]])
        if target.label in _DISPLAY_TESTS:
            if not local_only or "no-cache" not in requirements or "no-local" in requirements:
                fail("%s requires uncached local display execution" % target.label)
        elif local_only:
            fail("%s requests local execution outside the display test allowlist" % target.label)
    return []

linux_test_venue = aspect(implementation = _linux_test_venue_impl)
