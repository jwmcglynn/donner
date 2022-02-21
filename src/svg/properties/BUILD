load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "properties",
    srcs = [
        "paint_server.cc",
        "presentation_attribute_parsing.cc",
        "property_parsing.cc",
        "property_registry.cc",
    ],
    hdrs = [
        "paint_server.h",
        "presentation_attribute_parsing.h",
        "property.h",
        "property_parsing.h",
        "property_registry.h",
    ],
    visibility = ["//src/svg:__subpackages__"],
    deps = [
        "//src/base/parser",
        "//src/css",
        "//src/css/parser",
        "//src/svg/components:document_context",
        "//src/svg/components:transform_component",
        "//src/svg/graph",
        "//src/svg/parser",
        "//src/svg/registry",
        "@frozen",
    ],
)
