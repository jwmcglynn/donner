load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "components",
    hdrs = [
        "ComputedTextComponent.h",
        "TextComponent.h",
        "TextPositioningComponent.h",
        "TextRootComponent.h",
    ],
    visibility = [
        "//donner/svg/components:__subpackages__",
    ],
    deps = [
        "//donner/svg/properties",
    ],
)

cc_library(
    name = "text_system",
    srcs = [
        "TextSystem.cc",
    ],
    hdrs = [
        "TextSystem.h",
    ],
    visibility = [
        "//donner/svg/components:__subpackages__",
    ],
    deps = [
        ":components",
        "//donner/base",
        "//donner/svg/components:components_core",
    ],
)
