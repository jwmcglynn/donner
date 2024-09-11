load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "properties",
    srcs = [
        "PaintServer.cc",
        "PresentationAttributeParsing.cc",
        "PropertyParsing.cc",
        "PropertyRegistry.cc",
        "RxRyProperties.cc",
    ],
    hdrs = [
        "PaintServer.h",
        "PresentationAttributeParsing.h",
        "PropertyParsing.h",
        "PropertyRegistry.h",
        "RxRyProperties.h",
    ],
    visibility = ["//donner/svg:__subpackages__"],
    deps = [
        ":property",
        "//donner/base/parser",
        "//donner/base/xml",
        "//donner/css",
        "//donner/css/parser",
        "//donner/svg/components:document_context",
        "//donner/svg/components/filter:filter_effect",
        "//donner/svg/components/layout:components",
        "//donner/svg/graph",
        "//donner/svg/parser",
        "//donner/svg/registry",
        "//donner/svg/registry:element_type",
        "@frozen",
    ],
)

cc_library(
    name = "property",
    hdrs = [
        "Property.h",
    ],
    visibility = ["//donner/svg:__subpackages__"],
    deps = [
        "//donner/base",
        "//donner/css",
    ],
)
