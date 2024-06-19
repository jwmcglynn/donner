##
## Main library
##

#
# Donner main library, packages together the SVG library and associated renderer.
#
cc_library(
    name = "donner",
    visibility = ["//visibility:public"],
    deps = [
        "//donner/svg/renderer",
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

# Logo SVGs, which are consumed by the renderer tests.
filegroup(
    name = "logo_svgs",
    srcs = glob([
        "*.svg",
    ]),
    visibility = [
        "//donner:__subpackages__",
    ],
)
