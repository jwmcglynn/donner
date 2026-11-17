"""The generated repository supplies immutable source archives for both consumer builds."""

load("@rules_shell//shell:sh_test.bzl", "sh_test")

def cmake_remote_consumers():
    for name, kernel in [("linux", "Linux"), ("macos", "Darwin")]:
        sh_test(
            name = name,
            srcs = ["worker.sh"],
            args = [kernel, "$(rlocationpath :inputs.tar)"],
            data = ["inputs.tar"],
            timeout = "eternal",
            target_compatible_with = ["@platforms//os:" + name],
        )
