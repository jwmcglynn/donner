load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "components",
    srcs = [
        "gradient_component.cc",
        "linear_gradient_component.cc",
        "radial_gradient_component.cc",
        "stop_component.cc",
    ],
    hdrs = [
        "gradient_component.h",
        "linear_gradient_component.h",
        "radial_gradient_component.h",
        "stop_component.h",
    ],
    visibility = [
        "//src/svg/components:__subpackages__",
    ],
    deps = [
        "//src/svg/components:components_core",
        "//src/svg/components/shadow:components",
        "//src/svg/properties",
        "//src/svg/registry",
    ],
)

cc_library(
    name = "gradient_system",
    srcs = [
        "gradient_system.cc",
    ],
    hdrs = [
        "gradient_system.h",
    ],
    visibility = [
        "//src/svg/components:__subpackages__",
    ],
    deps = [
        ":components",
        "//src/svg/components:components_core",
        "//src/svg/components/shadow:components",
        "//src/svg/components/style:style_system",
        "//src/svg/properties",
        "//src/svg/registry",
    ],
)
