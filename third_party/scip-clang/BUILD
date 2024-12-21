load("@bazel_skylib//rules:native_binary.bzl", "native_binary")
load("@donner_tools_py_deps//:requirements.bzl", "requirement")
load("@rules_python//python:defs.bzl", "py_binary")

py_binary(
    name = "fetch_new_version",
    srcs = ["fetch_new_version.py"],
    deps = [
        requirement("requests"),
    ],
)

native_binary(
    name = "scip-clang-bin",
    src = select({
        "@platforms//os:osx": "@scip-clang-darwin-repo//file",
        "@platforms//os:linux": "@scip-clang-linux-repo//file",
        "//conditions:default": None,
    }),
    out = "scip-clang-bin",
)

genrule(
    name = "scip-clang-runner",
    outs = ["scip-clang-runner.sh"],
    cmd = """
    	echo '#!/usr/bin/env bash' > "$(OUTS)"
        echo 'cd "$${BUILD_WORKSPACE_DIRECTORY}"' >> "$(OUTS)"
        echo '$(location :scip-clang-bin) "$$@"' >> "$(OUTS)"
    """,
    executable = True,
    tools = [":scip-clang-bin"],
)

sh_binary(
    name = "scip-clang",
    srcs = ["scip-clang-runner.sh"],
    visibility = ["//visibility:public"],
)
