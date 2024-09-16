CORE_LINKOPTS = select({
    "@platforms//os:android": [
        "-landroid",
        "-ldl",
        "-llog",  # Provides __android_log_vprint, needed by //src/ports/SkDebug_android.cpp.
    ],
    "//conditions:default": [],
})

OPT_LEVEL = select({
    "//bazel/common_config_settings:debug_build": [],
    "//bazel/common_config_settings:fast_build_linux": [
        "-Wl,--strip-debug",
    ],
    "//bazel/common_config_settings:fast_build_mac": [],
    "//bazel/common_config_settings:release_build_mac": [
        "-dead_strip",
    ],
    "//bazel/common_config_settings:release_build_linux": [
        "-Wl,--gc-sections",
        "-Wl,--strip-all",
    ],
    "//conditions:default": [],
})

DEFAULT_LINKOPTS = CORE_LINKOPTS + OPT_LEVEL
