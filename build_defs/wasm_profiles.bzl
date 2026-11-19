"""Configuration used by the shipped editor and renderer Wasm CI roots."""

WASM_OPTION_INPUTS = ["//command_line_option:" + flag for flag in ["compilation_mode", "copt", "cxxopt", "linkopt"]]
WASM_OPTION_OUTPUTS = WASM_OPTION_INPUTS + [
    "//build_defs:disable_perf_opt_transition",
    "//donner/editor/wasm:enable_wasm",
    "//donner/svg/renderer/wasm:enable_wasm",
    "//donner/svg/renderer:renderer_backend",
    "//donner/svg/renderer:text",
    "//donner/svg/renderer:text_full",
    "//donner/svg/renderer/geode:enable_geode",
    "//donner/svg/renderer/geode:browser_backend",
]

def wasm_profile_options(settings, profile, browser_backend = True):
    """Mirror the three existing Wasm configurations at the artifact's graph edge."""
    editor = profile == "editor-wasm"
    geode = profile != "wasm-size"
    options = dict(settings)
    options.update({
        "//build_defs:disable_perf_opt_transition": True,
        "//donner/editor/wasm:enable_wasm": editor,
        "//donner/svg/renderer/wasm:enable_wasm": True,
        "//donner/svg/renderer:renderer_backend": "geode" if geode else "tiny_skia",
        "//donner/svg/renderer:text": editor,
        "//donner/svg/renderer:text_full": editor,
        "//donner/svg/renderer/geode:enable_geode": geode,
        "//donner/svg/renderer/geode:browser_backend": geode and browser_backend,
    })
    if profile != "wasm-geode":
        options["//command_line_option:compilation_mode"] = "opt"
    compile_flags = (["-pthread"] if editor else []) + (["-Oz"] if profile != "wasm-geode" else [])
    link_flags = compile_flags + (["-sBINARYEN_EXTRA_PASSES=-Oz", "--closure=1", "-sFILESYSTEM=0"] if not geode else [])
    for flag, additions in [("copt", compile_flags), ("cxxopt", ["-fconstexpr-steps=10000000"]), ("linkopt", link_flags)]:
        key = "//command_line_option:" + flag
        options[key] = settings[key] + [item for item in additions if item not in settings[key]]
    return options
