load("//build_defs:embed_resources.bzl", "embed_resources")

embed_resources(
    name = "public-sans",
    header_output = "embed_resources/PublicSansFont.h",
    resources = {
        "kPublicSansMediumOtf": "PublicSans-Medium.otf",
    },
    visibility = [
        "//donner:__subpackages__",
    ],
)
