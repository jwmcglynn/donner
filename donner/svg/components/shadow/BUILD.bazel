load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "components",
    hdrs = [
        "ComputedShadowTreeComponent.h",
        "OffscreenShadowTreeComponent.h",
        "ShadowBranch.h",
        "ShadowEntityComponent.h",
        "ShadowTreeComponent.h",
    ],
    visibility = [
        "//donner/svg/components:__subpackages__",
    ],
    deps = [
        "//donner/base",
        "//donner/svg/graph",
    ],
)

cc_library(
    name = "shadow_tree_system",
    srcs = [
        "ShadowTreeSystem.cc",
    ],
    hdrs = [
        "ShadowTreeSystem.h",
    ],
    visibility = [
        "//donner/svg/components:__subpackages__",
    ],
    deps = [
        ":components",
        "//donner/svg/components:components_core",
        "//donner/svg/components/style:components",
    ],
)
