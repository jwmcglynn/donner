"""Product variant build settings, and the one configuration every native variant shares.

Tests run in several product variants (renderer backend, text tier, Darwin test profile), each in
its own configuration, so every library below a variant test compiles once per variant even when
its commands are identical. `donner_cc_library` compiles a library's own sources once per platform
by default (see variant_neutral.bzl), resetting the settings below at its compile edge.

The native test-variant transitions are declared through `variant_transition`, which fails at load
time when an output is neither a variant setting here nor a platform option, so a new variant
setting cannot be left out of the reset. build_defs/check_variant_transitions.py lists every other
`transition()` that writes one of these settings, with the reason it stays outside, and fails on an
unreviewed one.
"""

# Each variant setting and its value in the variant-neutral configuration: the value of the Linux
# CI root configuration, so Linux neutral compiles share that configuration.
VARIANT_SETTINGS = {
    "//build_defs:disable_backend_test_transition": False,
    "//build_defs:metal_shader_validation": "on",
    "//donner/svg/renderer:renderer_backend": "tiny_skia",
    "//donner/svg/renderer:text": True,
    "//donner/svg/renderer:text_full": False,
    "//donner/svg/renderer/geode:enable_geode": False,
}

# Packages, with their subpackages, whose donner_cc_library targets compile in place by default
# because they build only in some product variants: compiled in the neutral configuration, their
# dependencies are incompatible, which would make the library incompatible in every variant and
# skip it silently. A library elsewhere opts out with `variant_neutral = False`.
VARIANT_DEPENDENT_PACKAGES = {
    "donner/editor": "The editor builds only in the full text tier.",
    "donner/svg/renderer/geode": "The Geode backend builds only with Geode enabled.",
}

def variant_neutral_default(package):
    """Whether donner_cc_library in `package` shares its compile unless it says otherwise.

    Args:
      package: The package name.

    Returns:
      False under a VARIANT_DEPENDENT_PACKAGES entry, True elsewhere.
    """
    for prefix in VARIANT_DEPENDENT_PACKAGES:
        if package == prefix or package.startswith(prefix + "/"):
            return False
    return True

# Options that select or configure the platform a variant runs on. Libraries keep its linker
# options, including caller overrides, after the Darwin transition removes the Linux runtime block.
PLATFORM_OPTIONS = [
    "//build_defs:linux_ci_runtime_link_dirs",
    "//build_defs:llvm_latest",
    "//command_line_option:action_env",
    "//command_line_option:extra_execution_platforms",
    "//command_line_option:host_action_env",
    "//command_line_option:host_platform",
    "//command_line_option:linkopt",
    "//command_line_option:platforms",
    "//command_line_option:test_env",
]

_PLATFORMS = "//command_line_option:platforms"
_DARWIN = str(Label("//tools/ci:remote_macos_aarch64"))

# The other Darwin CI platforms inherit this one and differ at most in the execution properties a
# test requests; the target CPU, OS and toolchain are the same. Ordinary Darwin tests run with
# shader validation off, so the neutral Darwin configuration is theirs.
_DARWIN_NEUTRAL = {
    _PLATFORMS: [_DARWIN],
    "//build_defs:metal_shader_validation": "off",
}
_NEUTRAL_PLATFORM_SETTINGS = {
    _DARWIN: _DARWIN_NEUTRAL,
    str(Label("//tools/ci:macos_ci")): _DARWIN_NEUTRAL,
    str(Label("//tools/ci:remote_macos_aarch64_gpu")): _DARWIN_NEUTRAL,
}

def unclassified_variant_outputs(outputs):
    """Returns the transition outputs that are neither variant settings nor platform options.

    Args:
      outputs: Transition output setting names.

    Returns:
      The unclassified outputs, in order.
    """
    return [output for output in outputs if output not in VARIANT_SETTINGS and output not in PLATFORM_OPTIONS]

def variant_transition(implementation, inputs, outputs):
    """Declares a transition that creates native product variants.

    Args:
      implementation: The transition implementation.
      inputs: Transition inputs.
      outputs: Transition outputs. Each must be in VARIANT_SETTINGS or PLATFORM_OPTIONS.

    Returns:
      The transition.
    """
    unclassified = unclassified_variant_outputs(outputs)
    if unclassified:
        fail("Add each new variant setting to VARIANT_SETTINGS in build_defs/variant_settings.bzl " +
             "(or to PLATFORM_OPTIONS if it selects the platform): " + ", ".join(unclassified))
    return transition(implementation = implementation, inputs = inputs, outputs = outputs)

def _variant_neutral_impl(settings, _attr):
    result = dict(VARIANT_SETTINGS)
    result[_PLATFORMS] = settings[_PLATFORMS]
    platforms = settings[_PLATFORMS]
    if len(platforms) == 1:
        result.update(_NEUTRAL_PLATFORM_SETTINGS.get(str(platforms[0]), {}))
    return result

variant_neutral_transition = transition(
    implementation = _variant_neutral_impl,
    inputs = [_PLATFORMS],
    outputs = VARIANT_SETTINGS.keys() + [_PLATFORMS],
)
