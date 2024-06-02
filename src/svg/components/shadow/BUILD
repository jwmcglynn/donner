load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "components",
    hdrs = [
        "computed_shadow_tree_component.h",
        "offscreen_shadow_tree_component.h",
        "shadow_branch.h",
        "shadow_entity_component.h",
        "shadow_tree_component.h",
    ],
    visibility = [
        "//src/svg/components:__subpackages__",
    ],
    deps = [
        "//src/svg/graph",
        "//src/svg/registry",
    ],
)

cc_library(
    name = "shadow_tree_system",
    srcs = [
        "shadow_tree_system.cc",
    ],
    hdrs = [
        "shadow_tree_system.h",
    ],
    visibility = [
        "//src/svg/components:__subpackages__",
    ],
    deps = [
        ":components",
        "//src/svg/components:components_core",
        "//src/svg/components/style:components",
    ],
)
