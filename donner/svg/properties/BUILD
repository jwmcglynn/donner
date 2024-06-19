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
        "Property.h",
        "PropertyParsing.h",
        "PropertyRegistry.h",
        "RxRyProperties.h",
    ],
    visibility = ["//donner/svg:__subpackages__"],
    deps = [
        "//donner/base/parser",
        "//donner/css",
        "//donner/css/parser",
        "//donner/svg/components:document_context",
        "//donner/svg/components:transform_component",
        "//donner/svg/components/filter:filter_effect",
        "//donner/svg/graph",
        "//donner/svg/parser",
        "//donner/svg/registry",
        "//donner/svg/xml:xml_qualified_name",
        "@frozen",
    ],
)
