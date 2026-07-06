"""Curated Google Fonts set embedded into Donner at build time.

SOURCE OF TRUTH for the embedded web-font catalog (Design 0013 W3).
Each entry is fetched at build time via `http_file` with a pinned URL + sha256,
so builds are deterministic and work offline after the first fetch. The font
bytes are NOT checked into the repo; only the pins live here.

To add/replace a family: add an entry below with a commit-pinned raw.githubusercontent
URL and its sha256 (compute with `shasum -a 256`). Keep GOOGLE_FONTS_COMMIT in sync
with the google/fonts commit the URLs point at.
"""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_file")

GOOGLE_FONTS_COMMIT = "e4572de925a4c3be12f1f9983ee0adbe1eb6e9fe"

# Curated starter set: 12 families spanning sans / serif / display / mono / script.
# Fields: family (display + CSS match name), category (FontCategory enum leaf),
# var (embed_resources C++ symbol), repo (http_file repo name), file, url, sha256, bytes.
GOOGLE_FONTS = [
    struct(
        family = "Inter",
        category = "SansSerif",
        var = "kGFInterTtf",
        repo = "gfont_inter",
        file = "Inter[opsz,wght].ttf",
        url = "https://raw.githubusercontent.com/google/fonts/e4572de925a4c3be12f1f9983ee0adbe1eb6e9fe/ofl/inter/Inter%5Bopsz%2Cwght%5D.ttf",
        sha256 = "29160a80ff49ddcab2c97711247e08b1fab27a484a329ce8b813d820dc559031",
        bytes = 876576,
    ),
    struct(
        family = "Open Sans",
        category = "SansSerif",
        var = "kGFOpenSansTtf",
        repo = "gfont_open_sans",
        file = "OpenSans[wdth,wght].ttf",
        url = "https://raw.githubusercontent.com/google/fonts/e4572de925a4c3be12f1f9983ee0adbe1eb6e9fe/ofl/opensans/OpenSans%5Bwdth%2Cwght%5D.ttf",
        sha256 = "36643644f318a812aab2d2ed3bb98f8cf0872527f835fe9398d95fe6b9adb878",
        bytes = 532636,
    ),
    struct(
        family = "Lato",
        category = "SansSerif",
        var = "kGFLatoTtf",
        repo = "gfont_lato",
        file = "Lato-Regular.ttf",
        url = "https://raw.githubusercontent.com/google/fonts/e4572de925a4c3be12f1f9983ee0adbe1eb6e9fe/ofl/lato/Lato-Regular.ttf",
        sha256 = "d636e4683231f931eda222d588e944d082bfd3bdba02f928bee461c0f185b251",
        bytes = 656568,
    ),
    struct(
        family = "Montserrat",
        category = "SansSerif",
        var = "kGFMontserratTtf",
        repo = "gfont_montserrat",
        file = "Montserrat[wght].ttf",
        url = "https://raw.githubusercontent.com/google/fonts/e4572de925a4c3be12f1f9983ee0adbe1eb6e9fe/ofl/montserrat/Montserrat%5Bwght%5D.ttf",
        sha256 = "0f7b311b2f3279e4eef9b2f968bcdbab6e28f4daeb1f049f4f278a902bcd82f7",
        bytes = 744936,
    ),
    struct(
        family = "Bitter",
        category = "Serif",
        var = "kGFBitterTtf",
        repo = "gfont_bitter",
        file = "Bitter[wght].ttf",
        url = "https://raw.githubusercontent.com/google/fonts/e4572de925a4c3be12f1f9983ee0adbe1eb6e9fe/ofl/bitter/Bitter%5Bwght%5D.ttf",
        sha256 = "ef2b9a711fb02f1e5823b34da1b7450e0fc76793b7d733a8b41006e24916d4a7",
        bytes = 328636,
    ),
    struct(
        family = "Lora",
        category = "Serif",
        var = "kGFLoraTtf",
        repo = "gfont_lora",
        file = "Lora[wght].ttf",
        url = "https://raw.githubusercontent.com/google/fonts/e4572de925a4c3be12f1f9983ee0adbe1eb6e9fe/ofl/lora/Lora%5Bwght%5D.ttf",
        sha256 = "822a6621ccbe8d97d20ac88c1c41f5615c9c2c202eaa75f272cd452aac6475a7",
        bytes = 212196,
    ),
    struct(
        family = "Playfair Display",
        category = "Serif",
        var = "kGFPlayfairDisplayTtf",
        repo = "gfont_playfair_display",
        file = "PlayfairDisplay[wght].ttf",
        url = "https://raw.githubusercontent.com/google/fonts/e4572de925a4c3be12f1f9983ee0adbe1eb6e9fe/ofl/playfairdisplay/PlayfairDisplay%5Bwght%5D.ttf",
        sha256 = "c40f2293766a503bc70cce9e512ef844a4ccb7cbcde792fe2ea31d191917d8d6",
        bytes = 300724,
    ),
    struct(
        family = "JetBrains Mono",
        category = "Monospace",
        var = "kGFJetbrainsMonoTtf",
        repo = "gfont_jetbrains_mono",
        file = "JetBrainsMono[wght].ttf",
        url = "https://raw.githubusercontent.com/google/fonts/e4572de925a4c3be12f1f9983ee0adbe1eb6e9fe/ofl/jetbrainsmono/JetBrainsMono%5Bwght%5D.ttf",
        sha256 = "48715a42ec242c21e9f02692891e147d022299a52e48d5e413e1a942193ffeda",
        bytes = 187208,
    ),
    struct(
        family = "Roboto Mono",
        category = "Monospace",
        var = "kGFRobotoMonoTtf",
        repo = "gfont_roboto_mono",
        file = "RobotoMono[wght].ttf",
        url = "https://raw.githubusercontent.com/google/fonts/e4572de925a4c3be12f1f9983ee0adbe1eb6e9fe/ofl/robotomono/RobotoMono%5Bwght%5D.ttf",
        sha256 = "66a80e79d17e4c7cabd162e2916578a4cc08fd19eef6e2a643305eae9c567b2b",
        bytes = 183700,
    ),
    struct(
        family = "Oswald",
        category = "Display",
        var = "kGFOswaldTtf",
        repo = "gfont_oswald",
        file = "Oswald[wght].ttf",
        url = "https://raw.githubusercontent.com/google/fonts/e4572de925a4c3be12f1f9983ee0adbe1eb6e9fe/ofl/oswald/Oswald%5Bwght%5D.ttf",
        sha256 = "5b38c246e255a12f5712d640d56bcced0472466fc68983d2d0410ec0457c2817",
        bytes = 172088,
    ),
    struct(
        family = "Bebas Neue",
        category = "Display",
        var = "kGFBebasNeueTtf",
        repo = "gfont_bebas_neue",
        file = "BebasNeue-Regular.ttf",
        url = "https://raw.githubusercontent.com/google/fonts/e4572de925a4c3be12f1f9983ee0adbe1eb6e9fe/ofl/bebasneue/BebasNeue-Regular.ttf",
        sha256 = "08e4623805102d819f58601e46e345648846075e363b2ceb23313c2d1c83ec73",
        bytes = 61400,
    ),
    struct(
        family = "Pacifico",
        category = "Handwriting",
        var = "kGFPacificoTtf",
        repo = "gfont_pacifico",
        file = "Pacifico-Regular.ttf",
        url = "https://raw.githubusercontent.com/google/fonts/e4572de925a4c3be12f1f9983ee0adbe1eb6e9fe/ofl/pacifico/Pacifico-Regular.ttf",
        sha256 = "5b6c0d5334a7bf77dea52b975c5a0c408878c0f7115ed5b6fb151f634b7bf701",
        bytes = 329380,
    ),
]

def _google_fonts_impl(_mctx):
    """Fetch each curated family via a pinned-hash http_file repo."""
    for f in GOOGLE_FONTS:
        http_file(
            name = f.repo,
            urls = [f.url],
            sha256 = f.sha256,
            downloaded_file_path = f.file,
        )

# Module extension: declared in //MODULE.bazel behind dev_dependency so BCR
# consumers never fetch these. When donner is the root module (local dev / CI)
# the extension runs and fetches every family exactly once, cached thereafter.
google_fonts = module_extension(implementation = _google_fonts_impl)

def embedded_resources_dict():
    """Return the {C++ var name: http_file label} map for embed_resources()."""
    return {f.var: "@" + f.repo + "//file" for f in GOOGLE_FONTS}

def _manifest_json_impl(ctx):
    entries = [
        {
            "family": f.family,
            "category": f.category,
            "var": f.var,
            "repo": f.repo,
            "file": f.file,
            "url": f.url,
            "sha256": f.sha256,
            "bytes": f.bytes,
        }
        for f in GOOGLE_FONTS
    ]
    ctx.actions.write(
        output = ctx.outputs.out,
        content = json.encode({"commit": GOOGLE_FONTS_COMMIT, "fonts": entries}),
    )

google_fonts_manifest = rule(
    implementation = _manifest_json_impl,
    attrs = {"out": attr.output(mandatory = True)},
    doc = "Write the GOOGLE_FONTS pin table to JSON for the integrity test.",
)

def _catalog_inc_impl(ctx):
    lines = [
        "// GENERATED by //third_party/google_fonts:google_fonts_catalog_inc. DO NOT EDIT.",
        "// Source of truth: third_party/google_fonts/fonts.bzl (GOOGLE_FONTS).",
        "// Consumed by donner/svg/resources/EmbeddedFontProvider.cc via the",
        "// DONNER_GF_ENTRY(family, category, span) x-macro.",
        "",
    ]
    for f in GOOGLE_FONTS:
        lines.append(
            "DONNER_GF_ENTRY(\"{family}\", {category}, ::donner::embedded::{var})".format(
                family = f.family,
                category = f.category,
                var = f.var,
            ),
        )
    ctx.actions.write(output = ctx.outputs.out, content = "\n".join(lines) + "\n")

google_fonts_catalog_inc = rule(
    implementation = _catalog_inc_impl,
    attrs = {"out": attr.output(mandatory = True)},
    doc = "Generate the DONNER_GF_ENTRY x-macro table for the embedded font provider.",
)
