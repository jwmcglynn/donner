"""Build transition for the Geode editor WebAssembly package."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

def _append_once(values, value):
    result = list(values)
    if value not in result:
        result.append(value)
    return result

def _editor_wasm_geode_transition_impl(settings, _attr):
    return {
        "//build_defs:disable_perf_opt_transition": True,
        "//donner/editor/wasm:enable_wasm": True,
        "//donner/svg/renderer/wasm:enable_wasm": True,
        "//donner/svg/renderer:renderer_backend": "geode",
        "//donner/svg/renderer:text": True,
        "//donner/svg/renderer:text_full": False,
        "//donner/svg/renderer/geode:enable_geode": True,
        "//command_line_option:compilation_mode": "opt",
        "//command_line_option:copt": _append_once(
            _append_once(
                settings["//command_line_option:copt"],
                "-pthread",
            ),
            "-Oz",
        ),
        "//command_line_option:cxxopt": _append_once(
            settings["//command_line_option:cxxopt"],
            "-fconstexpr-steps=10000000",
        ),
        "//command_line_option:linkopt": _append_once(
            _append_once(
                settings["//command_line_option:linkopt"],
                "-pthread",
            ),
            "-Oz",
        ),
    }

_editor_wasm_geode_transition = transition(
    implementation = _editor_wasm_geode_transition_impl,
    inputs = [
        "//command_line_option:copt",
        "//command_line_option:cxxopt",
        "//command_line_option:linkopt",
    ],
    outputs = [
        "//build_defs:disable_perf_opt_transition",
        "//donner/editor/wasm:enable_wasm",
        "//donner/svg/renderer/wasm:enable_wasm",
        "//donner/svg/renderer:renderer_backend",
        "//donner/svg/renderer:text",
        "//donner/svg/renderer:text_full",
        "//donner/svg/renderer/geode:enable_geode",
        "//command_line_option:compilation_mode",
        "//command_line_option:copt",
        "//command_line_option:cxxopt",
        "//command_line_option:linkopt",
    ],
)

def _editor_wasm_geode_transitioned_target_impl(ctx):
    dep = ctx.attr.dep
    if type(dep) == "list":
        if len(dep) != 1:
            fail("Geode editor WASM transition produced {} targets, expected 1".format(len(dep)))
        dep = dep[0]

    return [DefaultInfo(files = dep[DefaultInfo].files)]

editor_wasm_geode_transitioned_target = rule(
    implementation = _editor_wasm_geode_transitioned_target_impl,
    attrs = {
        "dep": attr.label(
            cfg = _editor_wasm_geode_transition,
            mandatory = True,
        ),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
)

def _editor_wasm_geode_only_guard_impl(ctx):
    editor_wasm_enabled = ctx.attr._editor_wasm_enabled[BuildSettingInfo].value
    renderer_backend = ctx.attr._renderer_backend[BuildSettingInfo].value
    if editor_wasm_enabled and renderer_backend != "geode":
        fail(
            "Donner editor Wasm is Geode-only; renderer_backend must be 'geode', got '{}'".format(
                renderer_backend,
            ),
        )

    output = ctx.actions.declare_file(ctx.label.name + ".txt")
    ctx.actions.write(output, "renderer_backend={}\n".format(renderer_backend))
    return [DefaultInfo(files = depset([output]))]

editor_wasm_geode_only_guard = rule(
    implementation = _editor_wasm_geode_only_guard_impl,
    attrs = {
        "_editor_wasm_enabled": attr.label(default = "//donner/editor/wasm:enable_wasm"),
        "_renderer_backend": attr.label(default = "//donner/svg/renderer:renderer_backend"),
    },
)

def _editor_wasm_config_probe_impl(ctx):
    output = ctx.actions.declare_file(ctx.label.name + ".txt")
    values = [
        "compilation_mode={}".format(ctx.var["COMPILATION_MODE"]),
        "copt_pthread={}".format("-pthread" in ctx.fragments.cpp.copts),
        "copt_oz={}".format("-Oz" in ctx.fragments.cpp.copts),
        "cxxopt_constexpr={}".format("-fconstexpr-steps=10000000" in ctx.fragments.cpp.cxxopts),
        "disable_perf_opt_transition={}".format(
            ctx.attr._disable_perf_opt_transition[BuildSettingInfo].value,
        ),
        "editor_wasm_enabled={}".format(ctx.attr._editor_wasm_enabled[BuildSettingInfo].value),
        "geode_enabled={}".format(ctx.attr._geode_enabled[BuildSettingInfo].value),
        "linkopt_pthread={}".format("-pthread" in ctx.fragments.cpp.linkopts),
        "linkopt_oz={}".format("-Oz" in ctx.fragments.cpp.linkopts),
        "renderer_backend={}".format(ctx.attr._renderer_backend[BuildSettingInfo].value),
        "renderer_wasm_enabled={}".format(
            ctx.attr._renderer_wasm_enabled[BuildSettingInfo].value,
        ),
        "text={}".format(ctx.attr._text[BuildSettingInfo].value),
        "text_full={}".format(ctx.attr._text_full[BuildSettingInfo].value),
    ]
    ctx.actions.write(output, "\n".join(values) + "\n")
    return [DefaultInfo(files = depset([output]))]

editor_wasm_config_probe = rule(
    implementation = _editor_wasm_config_probe_impl,
    attrs = {
        "_disable_perf_opt_transition": attr.label(
            default = "//build_defs:disable_perf_opt_transition",
        ),
        "_editor_wasm_enabled": attr.label(default = "//donner/editor/wasm:enable_wasm"),
        "_geode_enabled": attr.label(default = "//donner/svg/renderer/geode:enable_geode"),
        "_renderer_backend": attr.label(default = "//donner/svg/renderer:renderer_backend"),
        "_renderer_wasm_enabled": attr.label(
            default = "//donner/svg/renderer/wasm:enable_wasm",
        ),
        "_text": attr.label(default = "//donner/svg/renderer:text"),
        "_text_full": attr.label(default = "//donner/svg/renderer:text_full"),
    },
    fragments = ["cpp"],
)

def _setting_values(linkopts, name):
    """Every value the link line supplies for one `-s<NAME>=` Emscripten setting.

    Matched by setting name rather than by exact string so a re-tuned value can
    never silently slip past the probe, and so a duplicated setting (two
    `-sINITIAL_MEMORY` flags whose winner is a last-one-wins accident) shows up
    as two entries instead of one.
    """
    prefix = "-s{}=".format(name)
    return [opt[len(prefix):] for opt in linkopts if opt.startswith(prefix)]

def _single_setting(linkopts, name):
    values = _setting_values(linkopts, name)
    if len(values) != 1:
        return "<{} values>".format(len(values))
    return values[0]

def _editor_wasm_runtime_options_probe_impl(ctx):
    output = ctx.actions.declare_file(ctx.label.name + ".txt")
    values = ctx.attr.linkopts
    initial_memory = _single_setting(values, "INITIAL_MEMORY")
    maximum_memory = _single_setting(values, "MAXIMUM_MEMORY")
    lines = [
        "asyncify={}".format("-sASYNCIFY" in values),
        "closure={}".format("--closure=1" in values),
        "closure_simple={}".format(
            "--closure-args=--compilation_level=SIMPLE_OPTIMIZATIONS" in values,
        ),
        "exports_ccall={}".format("-sEXPORTED_RUNTIME_METHODS=ccall" in values),
        # the single-canvas architecture single-canvas whole-app contract.
        "proxy_to_pthread={}".format("-sPROXY_TO_PTHREAD" in values),
        "offscreencanvases_to_pthread={}".format(
            _single_setting(values, "OFFSCREENCANVASES_TO_PTHREAD"),
        ),
        "offscreencanvas_support={}".format(_single_setting(values, "OFFSCREENCANVAS_SUPPORT")),
        # Fixed linear memory: growth off, and initial == maximum.
        "memory_growth={}".format(_single_setting(values, "ALLOW_MEMORY_GROWTH")),
        "initial_memory={}".format(initial_memory),
        "maximum_memory={}".format(maximum_memory),
        "memory_is_fixed={}".format(initial_memory == maximum_memory),
        # One slot for the app pthread, one for AsyncRenderer's raster thread.
        "pthread_pool_size={}".format(_single_setting(values, "PTHREAD_POOL_SIZE")),
    ]
    ctx.actions.write(output, "\n".join(lines) + "\n")
    return [DefaultInfo(files = depset([output]))]

editor_wasm_runtime_options_probe = rule(
    implementation = _editor_wasm_runtime_options_probe_impl,
    attrs = {
        "linkopts": attr.string_list(mandatory = True),
    },
)
