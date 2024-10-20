load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "components",
    hdrs = [
        "ComputedStyleComponent.h",
        "DoNotInheritFillOrStrokeTag.h",
        "StyleComponent.h",
    ],
    visibility = [
        "//donner/svg/components:__subpackages__",
    ],
    deps = [
        "//donner/svg/properties",
    ],
)

cc_library(
    name = "style_system",
    srcs = [
        "StyleSystem.cc",
    ],
    hdrs = [
        "StyleSystem.h",
    ],
    visibility = [
        "//donner/svg/components:__subpackages__",
    ],
    deps = [
        ":components",
        "//donner/base",
        "//donner/svg/components:components_core",
        "//donner/svg/components/shadow:components",
    ],
)
