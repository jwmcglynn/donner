load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "svg_viewer",
    srcs = ["svg_viewer.cc"],
    target_compatible_with = select({
        "//build_defs:gui_supported": [
            "@platforms//os:macos",
        ],
        "//conditions:default": [
            "@platforms//:incompatible",
        ],
    }),
    deps = [
        "@com_google_absl//absl/debugging:failure_signal_handler",
        "@com_google_absl//absl/debugging:symbolize",
        "@donner",
        "@donner//third_party/glad",
        "@glfw",
        "@imgui",
    ],
)
