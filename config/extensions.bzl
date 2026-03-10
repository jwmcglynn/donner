"""Donner module extension for build-time feature configuration.

Usage in a downstream project's MODULE.bazel:

    donner = use_extension("@donner//config:extensions.bzl", "donner")
    donner.configure(
        renderer = "tiny_skia",
        text = True,
    )
    use_repo(donner, "donner_config")

All options have built-in defaults, so calling donner.configure() with no
arguments (or not calling it at all) produces the same result as the
hardcoded defaults.
"""

_DEFAULTS = dict(
    renderer = "tiny_skia",
    text = True,
    text_woff2 = True,
    text_shaping = False,
    use_coretext = False,
    use_fontconfig = False,
)

_configure = tag_class(attrs = {
    "renderer": attr.string(default = _DEFAULTS["renderer"], values = ["skia", "tiny_skia"]),
    "text": attr.bool(default = _DEFAULTS["text"]),
    "text_woff2": attr.bool(default = _DEFAULTS["text_woff2"]),
    "text_shaping": attr.bool(default = _DEFAULTS["text_shaping"]),
    "use_coretext": attr.bool(default = _DEFAULTS["use_coretext"]),
    "use_fontconfig": attr.bool(default = _DEFAULTS["use_fontconfig"]),
})

def _donner_config_repo_impl(rctx):
    rctx.file("BUILD.bazel", 'exports_files(["config.bzl"])\n')
    rctx.file("config.bzl", """\
DONNER_CONFIG = {{
    "renderer": {renderer},
    "text": {text},
    "text_woff2": {text_woff2},
    "text_shaping": {text_shaping},
    "use_coretext": {use_coretext},
    "use_fontconfig": {use_fontconfig},
}}
""".format(
        renderer = repr(rctx.attr.renderer),
        text = repr(rctx.attr.text),
        text_woff2 = repr(rctx.attr.text_woff2),
        text_shaping = repr(rctx.attr.text_shaping),
        use_coretext = repr(rctx.attr.use_coretext),
        use_fontconfig = repr(rctx.attr.use_fontconfig),
    ))

_donner_config_repo = repository_rule(
    implementation = _donner_config_repo_impl,
    attrs = {
        "renderer": attr.string(default = _DEFAULTS["renderer"]),
        "text": attr.bool(default = _DEFAULTS["text"]),
        "text_woff2": attr.bool(default = _DEFAULTS["text_woff2"]),
        "text_shaping": attr.bool(default = _DEFAULTS["text_shaping"]),
        "use_coretext": attr.bool(default = _DEFAULTS["use_coretext"]),
        "use_fontconfig": attr.bool(default = _DEFAULTS["use_fontconfig"]),
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
                    text_woff2 = tag.text_woff2,
                    text_shaping = tag.text_shaping,
                    use_coretext = tag.use_coretext,
                    use_fontconfig = tag.use_fontconfig,
                )

    _donner_config_repo(name = "donner_config", **cfg)

donner = module_extension(
    implementation = _donner_impl,
    tag_classes = {"configure": _configure},
)
