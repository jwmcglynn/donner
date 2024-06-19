load("@hedron_compile_commands//:refresh_compile_commands.bzl", "refresh_compile_commands")

refresh_compile_commands(
    name = "refresh_compile_commands",
    targets = "//donner/...",
)

# Only generate compile commands for the src/ directory, which excludes third_party/.
refresh_compile_commands(
    name = "sourcegraph_compile_commands",
    exclude_external_sources = True,
    targets = "//donner/...",
)
