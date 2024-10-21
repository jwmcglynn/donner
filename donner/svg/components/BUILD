load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "svg_document_context",
    srcs = [
        "SVGDocumentContext.cc",
    ],
    hdrs = [
        "IdComponent.h",
        "SVGDocumentContext.h",
    ],
    visibility = [
        "//donner/svg:__subpackages__",
    ],
    deps = [
        "//donner/base",
    ],
)

cc_library(
    name = "components_core",
    srcs = [
        "StylesheetComponent.cc",
    ],
    hdrs = [
        "ClassComponent.h",
        "ElementTypeComponent.h",
        "EvaluatedReferenceComponent.h",
        "PathLengthComponent.h",
        "PreserveAspectRatioComponent.h",
        "RenderingBehaviorComponent.h",
        "RenderingInstanceComponent.h",
        "StylesheetComponent.h",
    ],
    visibility = [
        "//donner/css:__pkg__",
        "//donner/svg:__subpackages__",
    ],
    deps = [
        ":svg_document_context",
        "//donner/base",
        "//donner/base/xml",
        "//donner/base/xml/components",
        "//donner/svg/components/layout:components",
        "//donner/svg/components/shadow:components",
        "//donner/svg/components/style:components",
        "//donner/svg/parser:parser_core",
        "//donner/svg/properties",
    ],
)

cc_library(
    name = "components",
    visibility = [
        "//donner/css:__pkg__",
        "//donner/svg:__subpackages__",
    ],
    deps = [
        ":components_core",
        "//donner/svg/components/filter:filter_system",
        "//donner/svg/components/layout:layout_system",
        "//donner/svg/components/paint:paint_system",
        "//donner/svg/components/resources:components",
        "//donner/svg/components/resources:resource_manager_context",
        "//donner/svg/components/shadow:components",
        "//donner/svg/components/shadow:shadow_tree_system",
        "//donner/svg/components/shape:shape_system",
    ],
)
