load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "core",
    srcs = [
        "PathSpline.cc",
        "PreserveAspectRatio.cc",
    ],
    hdrs = [
        "ClipPathUnits.h",
        "ClipRule.h",
        "CssTransform.h",
        "Display.h",
        "FillRule.h",
        "Gradient.h",
        "MarkerOrient.h",
        "MarkerUnits.h",
        "MaskUnits.h",
        "Overflow.h",
        "PathSpline.h",
        "Pattern.h",
        "PointerEvents.h",
        "PreserveAspectRatio.h",
        "Stroke.h",
        "Visibility.h",
    ],
    visibility = [
        "//donner/svg:__subpackages__",
        "//donner/svg/renderer:__subpackages__",
    ],
    deps = [
        "//donner/base",
        "//donner/css:core",
    ],
)
