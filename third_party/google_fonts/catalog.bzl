"""Generate one WOFF2 catalog for native embedding and deferred web delivery."""

load(":fonts.bzl", "GOOGLE_FONTS")

def _catalog_impl(ctx):
    assets = ctx.actions.declare_directory(ctx.label.name + ".assets")
    metadata = ctx.actions.declare_file("embed_resources/GoogleFontsCatalog.inc")
    header = ctx.actions.declare_file("embed_resources/GoogleFontsData.h")
    manifest = ctx.actions.declare_file("catalog-fonts.json")
    notices = ctx.actions.declare_file("CatalogFontNotices.txt")
    native_sources = [ctx.actions.declare_file("native/" + font.repo + ".cc") for font in GOOGLE_FONTS]
    request = ctx.actions.declare_file(ctx.label.name + ".request.json")
    ctx.actions.write(request, json.encode({
        "inputs": [file.path for file in ctx.files.fonts],
        "licenses": [file.path for file in ctx.files.licenses],
        "native_sources": [file.path for file in native_sources],
        "codec_sources": [{"name": file.basename, "path": file.path} for file in ctx.files.codec_sources],
    }))
    args = ctx.actions.args()
    args.add_all([
        "--request",
        request.path,
        "--pins",
        ctx.file.pins.path,
        "--encoder",
        ctx.executable.encoder.path,
        "--encoder-source",
        ctx.file.encoder_source.path,
        "--dependency-pins",
        ctx.file.dependency_pins.path,
        "--module",
        ctx.file.module.path,
        "--assets",
        assets.path,
        "--metadata",
        metadata.path,
        "--header",
        header.path,
        "--manifest",
        manifest.path,
        "--notices",
        notices.path,
    ])
    ctx.actions.run(
        executable = ctx.executable._generator,
        arguments = [args],
        inputs = depset(
            ctx.files.fonts + ctx.files.licenses + ctx.files.codec_sources + [
                request,
                ctx.file.pins,
                ctx.file.encoder_source,
                ctx.file.dependency_pins,
                ctx.file.module,
            ],
        ),
        tools = [ctx.attr.encoder[DefaultInfo].files_to_run],
        outputs = native_sources + [assets, metadata, header, manifest, notices],
        mnemonic = "CatalogWoff2",
        progress_message = "Encoding pinned catalog fonts as WOFF2",
    )
    return [
        DefaultInfo(files = depset([assets])),
        OutputGroupInfo(
            metadata = depset([metadata]),
            native_header = depset([header]),
            native_sources = depset(native_sources),
            manifest = depset([manifest]),
            notices = depset([notices]),
        ),
    ]

google_fonts_woff2_catalog = rule(
    implementation = _catalog_impl,
    attrs = {
        "fonts": attr.label_list(allow_files = True, mandatory = True),
        "licenses": attr.label_list(allow_files = [".txt"], mandatory = True),
        "pins": attr.label(allow_single_file = True, mandatory = True),
        "encoder": attr.label(executable = True, cfg = "exec", mandatory = True),
        "encoder_source": attr.label(allow_single_file = True, mandatory = True),
        "codec_sources": attr.label_list(
            allow_files = True,
            mandatory = True,
        ),
        "dependency_pins": attr.label(
            allow_single_file = True,
            default = "//third_party:bazel/non_bcr_deps.bzl",
        ),
        "module": attr.label(allow_single_file = True, default = "//:MODULE.bazel"),
        "_generator": attr.label(
            executable = True,
            cfg = "exec",
            default = ":catalog_generator",
        ),
    },
)

def catalog_font_labels():
    """Return pinned sfnt labels in manifest order."""
    return ["@" + font.repo + "//file" for font in GOOGLE_FONTS]

def catalog_license_labels():
    """Return checked-in notices in manifest order."""
    return ["licenses/" + font.repo + ".txt" for font in GOOGLE_FONTS]

def catalog_asset_tests(target_compatible_with):
    """Test each font independently so failures and expensive encodes stay localized."""
    tests = []
    for font in GOOGLE_FONTS:
        name = "catalog_woff2_" + font.repo[len("gfont_"):] + "_test"
        source = "@" + font.repo + "//file"
        native.py_test(
            name = name,
            size = "small",
            srcs = ["catalog_generate.py", "catalog_woff2_assets_test.py"],
            main = "catalog_woff2_assets_test.py",
            imports = ["."],
            args = [
                "--manifest",
                "$(rootpath :catalog_manifest)",
                "--assets",
                "$(rootpath :google_fonts_woff2_assets)",
                "--repo",
                font.repo,
                "--source",
                "$(rootpath " + source + ")",
                "--encoder",
                "$(rootpath :catalog_woff2_tool)",
                "--compare",
                "$(rootpath :catalog_font_compare)",
            ],
            data = [
                source,
                ":catalog_manifest",
                ":google_fonts_woff2_assets",
                ":catalog_woff2_tool",
                ":catalog_font_compare",
            ],
            target_compatible_with = target_compatible_with,
        )
        tests.append(":" + name)
    native.test_suite(
        name = "catalog_woff2_assets_test",
        tests = tests + [":catalog_metadata_test", ":catalog_generator_test", ":integrity_test"],
        visibility = ["//visibility:public"],
    )
