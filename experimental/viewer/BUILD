load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "svg_viewer",
    srcs = ["svg_viewer.cc"],
    target_compatible_with = [
        "@platforms//os:macos",
    ],
    deps = [
        "@com_google_absl//absl/debugging:failure_signal_handler",
        "@com_google_absl//absl/debugging:symbolize",
        "@donner",
        "@donner//third_party/glad",
        "@glfw",
        "@imgui",
    ],
)
