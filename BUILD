load("@hedron_compile_commands//:refresh_compile_commands.bzl", "refresh_compile_commands")

refresh_compile_commands(
    name = "refresh_compile_commands",
)

config_setting(
    name = "debug_build",
    values = {
        "compilation_mode": "dbg",
    },
    visibility = ["//visibility:public"],
)
