##
## Main library
##

#
# Donner main library, packages together the SVG library and associated renderer.
#
cc_library(
    name = "donner",
    include_prefix = "donner",
    strip_include_prefix = "src",
    visibility = ["//visibility:public"],
    deps = [
        "//src/svg/renderer",
    ],
)

##
## Devtools
##

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

filegroup(
    name = "docs_files",
    srcs = glob([
        "docs/**/*.md",
        "docs/**/*.svg",
        "docs/**/*.png",
    ]),
    visibility = ["//tools:__pkg__"],
)

exports_files(
    [".hdoc.toml"],
    visibility = ["//visibility:public"],
)
