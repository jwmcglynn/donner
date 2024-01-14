load("@hedron_compile_commands//:refresh_compile_commands.bzl", "refresh_compile_commands")
load("//tools/aspects:hdoc_aspect.bzl", "hdoc_generate")
load("//tools/http:serve_http.bzl", "serve_http")

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

hdoc_generate(
    name = "hdoc",
    files = glob([
        "docs/**/*.md",
        "docs/**/*.svg",
        "docs/**/*.png",
    ]),
    tags = [
        "docs",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//src/svg",
    ],
)

# Build and launch a simple webserver to view the generated documentation.
# bazel run //:hdoc_serve
serve_http(
    name = "hdoc_serve",
    dir = ":hdoc",
    tags = [
        "docs",
    ],
)
