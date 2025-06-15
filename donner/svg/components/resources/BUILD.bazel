load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "components",
    hdrs = [
        "ImageComponent.h",
    ],
    visibility = [
        "//donner/svg/components:__subpackages__",
    ],
    deps = [
        "//donner/base",
        "//donner/svg/properties",
        "//donner/svg/resources:image_resource",
    ],
)

cc_library(
    name = "resource_manager_context",
    srcs = [
        "ResourceManagerContext.cc",
    ],
    hdrs = [
        "ResourceManagerContext.h",
    ],
    visibility = [
        "//donner/svg/components:__subpackages__",
    ],
    deps = [
        ":components",
        "//donner/svg/resources:image_loader",
        "//donner/svg/resources:resource_loader_interface",
    ],
)
