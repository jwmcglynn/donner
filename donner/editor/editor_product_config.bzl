"""Full-text editor products and tests, independent of the core SVG feature tier."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("//build_defs:rules.bzl", "donner_cc_binary", "donner_cc_fuzzer", "donner_cc_test", "donner_multi_transitioned_binary", "donner_multi_transitioned_test")

EDITOR_RENDERER_BACKEND = "geode"
EDITOR_TEXT = "true"
EDITOR_TEXT_FULL = "true"

_FULL_TEXT_GUARD = "//donner/editor:requires_text_true_and_text_full_true"
_TEST_FORWARDED_ATTRS = ["size", "timeout", "shard_count", "flaky", "local", "target_compatible_with", "visibility"]

def editor_full_text_compatible_with():
    """Reject direct implementation use outside the editor's full-text configuration."""
    return select({
        "//donner/svg/renderer:text_full_enabled": [],
        "//conditions:default": ["@platforms//:incompatible"],
    })

def _editor_full_text_guard_impl(ctx):
    if not ctx.attr._text[BuildSettingInfo].value or not ctx.attr._text_full[BuildSettingInfo].value:
        fail("Editor implementation requires --//donner/svg/renderer:text=true and --//donner/svg/renderer:text_full=true; use the public editor entrypoint to select these automatically")
    return [CcInfo()]

editor_full_text_guard = rule(
    implementation = _editor_full_text_guard_impl,
    attrs = {
        "_text": attr.label(default = "//donner/svg/renderer:text"),
        "_text_full": attr.label(default = "//donner/svg/renderer:text_full"),
    },
)

def editor_cc_binary(name, deps = [], tags = [], renderer_backend = "inherit", **kwargs):
    """Create an editor executable that always enables both text feature flags."""
    implementation_kwargs = dict(kwargs)
    implementation_kwargs["visibility"] = ["//visibility:private"]
    donner_cc_binary(
        name = name + "_impl",
        deps = deps + [_FULL_TEXT_GUARD],
        tags = tags + ["manual"],
        **implementation_kwargs
    )
    donner_multi_transitioned_binary(
        name = name,
        dep = ":" + name + "_impl",
        renderer_backend = renderer_backend,
        full_text_only = True,
        text = EDITOR_TEXT,
        text_full = EDITOR_TEXT_FULL,
        tags = tags,
        **{key: kwargs[key] for key in ["target_compatible_with", "visibility", "testonly"] if key in kwargs}
    )

def editor_cc_test(name, deps = [], tags = [], variants = None, renderer_backend = "inherit", opens_gpu_device = False, remote_parallel_ci = False, **kwargs):
    """Keep each editor test and backend variant on the full-text product configuration."""
    implementation_kwargs = dict(kwargs)
    implementation_kwargs["visibility"] = ["//visibility:private"]
    donner_cc_test(
        name = name + "_impl",
        deps = deps + [_FULL_TEXT_GUARD],
        tags = tags + ["manual"],
        **implementation_kwargs
    )
    forwarded = {key: kwargs[key] for key in _TEST_FORWARDED_ATTRS if key in kwargs}
    donner_multi_transitioned_test(
        name = name,
        dep = ":" + name + "_impl",
        renderer_backend = renderer_backend,
        full_text_only = True,
        text = EDITOR_TEXT,
        text_full = EDITOR_TEXT_FULL,
        fixed_args = kwargs.get("args", []),
        argument_files = kwargs.get("data", []),
        opens_gpu_device = opens_gpu_device,
        remote_parallel_ci = remote_parallel_ci,
        tags = tags,
        **forwarded
    )
    for variant in variants or []:
        if variant not in ["geode", "text_full"]:
            fail("Editor tests require full text; unsupported variant '{}'".format(variant))
        donner_multi_transitioned_test(
            name = name + "_" + variant,
            dep = ":" + name + "_impl",
            renderer_backend = "geode",
            full_text_only = True,
            text = EDITOR_TEXT,
            text_full = EDITOR_TEXT_FULL,
            fixed_args = kwargs.get("args", []),
            argument_files = kwargs.get("data", []),
            opens_gpu_device = opens_gpu_device,
            remote_parallel_ci = remote_parallel_ci,
            tags = tags + ["variant_" + variant],
            **forwarded
        )

def editor_perf_cc_test(name, correctness_srcs, wallclock_srcs, srcs = [], tags = [], wallclock_tags = [], **kwargs):
    """Keep correctness and opt-in performance measurements on the editor configuration."""
    if not correctness_srcs or not wallclock_srcs:
        fail("Editor performance tests require correctness and wallclock sources")
    editor_cc_test(
        name = name + "_correctness",
        srcs = srcs + correctness_srcs,
        tags = [tag for tag in tags if tag not in ["manual", "perf"]],
        **kwargs
    )
    editor_cc_test(
        name = name + "_wallclock",
        srcs = srcs + wallclock_srcs,
        tags = depset(tags + wallclock_tags + ["manual", "perf"]).to_list(),
        **kwargs
    )

def editor_cc_fuzzer(name, deps = [], **kwargs):
    """Keep editor corpus replay and mutation coverage on the full-text configuration."""
    donner_cc_fuzzer(
        name = name,
        deps = deps + [_FULL_TEXT_GUARD],
        full_text_only = True,
        **kwargs
    )
