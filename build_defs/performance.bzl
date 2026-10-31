"""Native speed settings for translation units that must survive size optimization."""

def native_speed_copts(optimization_level = "O2", allow_debug = False):
    """Returns native-only speed flags without changing dependency configurations.

    Args:
      optimization_level: Explicit O2 or O3 optimization for the target's own sources.
      allow_debug: Leaves native dbg compilation at its caller-selected level.

    Returns:
      Configured compile options; WebAssembly retains its caller-selected size policy.
    """
    if optimization_level not in ("O2", "O3"):
        fail("Native speed optimization must be O2 or O3, got {}".format(optimization_level))
    conditions = {
        Label("//build_defs:wasm32_platform"): [],
        "//conditions:default": ["-" + optimization_level],
    }
    if allow_debug:
        conditions[Label("//build_defs:debug_compilation")] = []
    return select(conditions)
