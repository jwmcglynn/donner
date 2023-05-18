load("@rules_cc//cc:defs.bzl", "cc_library")

VERSION_GFM = "0"

VERSION_MAJOR = "0"

VERSION_MINOR = "0"

VERSION_PATCH = "0"

genrule(
    name = "generated_version_header",
    srcs = ["src/cmark-gfm_version.h.in"],
    outs = ["cmark-gfm_version.h"],
    cmd = ("sed -e 's/@PROJECT_VERSION_MAJOR@/{major}/g'" +
           " -e 's/@PROJECT_VERSION_MINOR@/{minor}/g'" +
           " -e 's/@PROJECT_VERSION_PATCH@/{patch}/g' " +
           "-e 's/@PROJECT_VERSION_GFM@/{gfm}/g' $< > $@").format(
        gfm = VERSION_GFM,
        major = VERSION_MAJOR,
        minor = VERSION_MINOR,
        patch = VERSION_PATCH,
    ),
)

cc_library(
    name = "extensions",
    srcs = [
        "extensions/autolink.c",
        "extensions/core-extensions.c",
        "extensions/ext_scanners.c",
        "extensions/strikethrough.c",
        "extensions/table.c",
        "extensions/tagfilter.c",
        "extensions/tasklist.c",
    ],
    hdrs = [
        "extensions/autolink.h",
        "extensions/cmark-gfm-core-extensions.h",
        "extensions/ext_scanners.h",
        "extensions/strikethrough.h",
        "extensions/table.h",
        "extensions/tagfilter.h",
        "extensions/tasklist.h",
    ],
    includes = ["extensions"],
    deps = [
        ":src",
    ],
)

cc_library(
    name = "src",
    srcs = [
        "src/arena.c",
        "src/blocks.c",
        "src/buffer.c",
        "src/cmark.c",
        "src/cmark_ctype.c",
        "src/commonmark.c",
        "src/footnotes.c",
        "src/houdini_href_e.c",
        "src/houdini_html_e.c",
        "src/houdini_html_u.c",
        "src/html.c",
        "src/inlines.c",
        "src/iterator.c",
        "src/latex.c",
        "src/linked_list.c",
        "src/man.c",
        "src/map.c",
        "src/node.c",
        "src/plaintext.c",
        "src/plugin.c",
        "src/references.c",
        "src/registry.c",
        "src/render.c",
        "src/scanners.c",
        "src/syntax_extension.c",
        "src/utf8.c",
        "src/xml.c",
    ],
    hdrs = [
        "src/buffer.h",
        "src/case_fold_switch.inc",
        "src/chunk.h",
        "src/cmark-gfm.h",
        "src/cmark-gfm-extension_api.h",
        "src/cmark_ctype.h",
        "src/entities.inc",
        "src/footnotes.h",
        "src/houdini.h",
        "src/html.h",
        "src/inlines.h",
        "src/iterator.h",
        "src/map.h",
        "src/node.h",
        "src/parser.h",
        "src/plugin.h",
        "src/references.h",
        "src/registry.h",
        "src/render.h",
        "src/scanners.h",
        "src/syntax_extension.h",
        "src/utf8.h",
        ":generated_version_header",
    ],
    defines = [
        "CMARK_GFM_STATIC_DEFINE",
        "CMARK_GFM_EXTENSIONS_STATIC_DEFINE",
    ],
    includes = ["src"],
    deps = [
        "@donner//third_party/cmark-gfm:generated_files",
    ],
)

cc_library(
    name = "cmark-gfm",
    visibility = ["//visibility:public"],
    deps = [
        ":extensions",
        ":src",
    ],
)
