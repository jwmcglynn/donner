load("@bazel_skylib//rules:common_settings.bzl", "string_flag")

string_flag(
    name = "llvm_latest",
    build_setting_default = "0",
)

config_setting(
    name = "fuzzers_enabled",
    flag_values = {
        ":llvm_latest": "1",
    },
)

config_setting(
    name = "gui_supported",
    flag_values = {
        ":llvm_latest": "0",
    },
)
