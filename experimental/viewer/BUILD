load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "svg_viewer",
    srcs = ["svg_viewer.cc"],
    target_compatible_with = [
        "@platforms//os:macos",
    ],
    deps = [
        "@donner",
        "@donner//third_party/glad",
        "@glfw",
        "@imgui",
    ],
)
