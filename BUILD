load("@hedron_compile_commands//:refresh_compile_commands.bzl", "refresh_compile_commands")

refresh_compile_commands(
    name = "refresh_compile_commands",
)

# Only generate compile commands for the src/ directory, which excludes third_party/.
refresh_compile_commands(
    name = "sourcegraph_compile_commands",
    exclude_external_sources = True,
    targets = "//src/...",
)

filegroup(
    name = "clang_tidy_config",
    srcs = [".clang-tidy"],
    visibility = ["//visibility:public"],
)

config_setting(
    name = "debug_build",
    values = {
        "compilation_mode": "dbg",
    },
    visibility = ["//visibility:public"],
)

exports_files(
    [".hdoc.toml"],
    visibility = ["//visibility:public"],
)
