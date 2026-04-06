"""Donner module extension for build-time feature configuration.

Usage in a downstream project's MODULE.bazel:

    donner = use_extension("@donner//config:extensions.bzl", "donner")
    donner.configure(
        renderer = "tiny_skia",
        text = True,
        text_full = True,
    )
    use_repo(donner, "donner_config")

All options have built-in defaults, so calling donner.configure() with no
arguments (or not calling it at all) produces the same result as the
hardcoded defaults.

Text rendering tiers:
  - text=False:              No text rendering.
  - text=True:               Simple text (stb_truetype only — TTF/OTF, kern-table kerning).
  - text=True, text_full=True: Full text (FreeType hinted outlines, HarfBuzz GSUB/GPOS, WOFF2).
"""

_DEFAULTS = dict(
    renderer = "tiny_skia",
    text = True,
    text_full = False,
)

_configure = tag_class(attrs = {
    "renderer": attr.string(default = _DEFAULTS["renderer"], values = ["skia", "tiny_skia"]),
    "text": attr.bool(default = _DEFAULTS["text"]),
    "text_full": attr.bool(default = _DEFAULTS["text_full"]),
})

def _donner_config_repo_impl(rctx):
    rctx.file("BUILD.bazel", 'exports_files(["config.bzl"])\n')
    rctx.file("config.bzl", """\
DONNER_CONFIG = {{
    "renderer": {renderer},
    "text": {text},
    "text_full": {text_full},
}}
""".format(
        renderer = repr(rctx.attr.renderer),
        text = repr(rctx.attr.text),
        text_full = repr(rctx.attr.text_full),
    ))

_donner_config_repo = repository_rule(
    implementation = _donner_config_repo_impl,
    attrs = {
        "renderer": attr.string(default = _DEFAULTS["renderer"]),
        "text": attr.bool(default = _DEFAULTS["text"]),
        "text_full": attr.bool(default = _DEFAULTS["text_full"]),
    },
)

def _donner_impl(module_ctx):
    cfg = dict(**_DEFAULTS)

    for mod in module_ctx.modules:
        for tag in mod.tags.configure:
            if mod.is_root:
                cfg = dict(
                    renderer = tag.renderer,
                    text = tag.text,
                    text_full = tag.text_full,
                )

    _donner_config_repo(name = "donner_config", **cfg)

donner = module_extension(
    implementation = _donner_impl,
    tag_classes = {"configure": _configure},
)
