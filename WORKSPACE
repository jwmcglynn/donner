"""
Donner Bazel workspace rules.
"""

workspace(name = "donner")

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository", "new_git_repository")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

##
## Toolchain
##

# BAZEL_TOOLCHAIN_TAG = "0.8.2"

# BAZEL_TOOLCHAIN_SHA = "0fc3a2b0c9c929920f4bed8f2b446a8274cad41f5ee823fd3faa0d7641f20db0"

# http_archive(
#     name = "com_grail_bazel_toolchain",
#     canonical_id = BAZEL_TOOLCHAIN_TAG,
#     sha256 = BAZEL_TOOLCHAIN_SHA,
#     strip_prefix = "bazel-toolchain-{tag}".format(tag = BAZEL_TOOLCHAIN_TAG),
#     url = "https://github.com/grailbio/bazel-toolchain/archive/refs/tags/{tag}.tar.gz".format(tag = BAZEL_TOOLCHAIN_TAG),
# )

git_repository(
    name = "com_grail_bazel_toolchain",
    #branch = "main",
    commit = "84b0a84c4b79f4f4eb795c7dd5c808406f0b7bb7",
    remote = "https://github.com/jwmcglynn/bazel-toolchain.git",
)

load("@com_grail_bazel_toolchain//toolchain:deps.bzl", "bazel_toolchain_dependencies")

bazel_toolchain_dependencies()

load("@com_grail_bazel_toolchain//toolchain:rules.bzl", "llvm_toolchain")

llvm_toolchain(
    name = "llvm_toolchain",
    llvm_version = "15.0.6",
)

load("@llvm_toolchain//:toolchains.bzl", "llvm_register_toolchains")

llvm_register_toolchains()

##
## Bazel and IntelliSense
##

# Hedron's Compile Commands Extractor for Bazel
# https://github.com/hedronvision/bazel-compile-commands-extractor
git_repository(
    name = "hedron_compile_commands",
    # branch = "main",
    commit = "b33a4b05c2287372c8e932c55ff4d3a37e6761ed",
    remote = "https://github.com/hedronvision/bazel-compile-commands-extractor.git",
    shallow_since = "1638167585 -0800",
)

load("@hedron_compile_commands//:workspace_setup.bzl", "hedron_compile_commands_setup")

hedron_compile_commands_setup()

## Developer tools

git_repository(
    name = "bazel_clang_tidy",
    commit = "674fac7640ae0469b7a58017018cb1497c26e2bf",
    remote = "https://github.com/erenon/bazel_clang_tidy.git",
)

##
## Third-party dependencies.
##

git_repository(
    name = "com_google_gtest",
    remote = "https://github.com/google/googletest",
    tag = "v1.14.0",
)

load("@com_google_gtest//:googletest_deps.bzl", "googletest_deps")

googletest_deps()

git_repository(
    name = "pixelmatch-cpp17",
    branch = "main",
    remote = "https://github.com/jwmcglynn/pixelmatch-cpp17",
)

new_local_repository(
    name = "css-parsing-tests",
    build_file = "third_party/BUILD.css-parsing-tests",
    path = "third_party/css-parsing-tests",
)

new_local_repository(
    name = "rapidxml_ns",
    build_file = "third_party/BUILD.rapidxml_ns",
    path = "third_party/rapidxml_ns",
)

git_repository(
    name = "entt",
    commit = "a03b88e0eb679ca0fbb79d9c54293e772b4f4a93",
    remote = "https://github.com/skypjack/entt",
)

new_local_repository(
    name = "frozen",
    build_file = "third_party/BUILD.frozen",
    path = "third_party/frozen",
)

new_git_repository(
    name = "nlohmann_json",
    build_file = "@//third_party:BUILD.nlohmann_json",
    remote = "https://github.com/nlohmann/json",
    tag = "v3.9.1",
)

git_repository(
    name = "bazel_skylib",
    commit = "560d7b2359aecb066d81041cb532b82d7354561b",
    remote = "https://github.com/bazelbuild/bazel-skylib",
)

git_repository(
    name = "range-v3",
    branch = "master",
    remote = "https://github.com/ericniebler/range-v3",
)

git_repository(
    name = "stb",
    branch = "master",
    init_submodules = True,
    remote = "https://github.com/nitronoid/rules_stb",
)

local_repository(
    name = "skia_user_config",
    path = "third_party/skia_user_config",
)

git_repository(
    name = "skia",
    commit = "7cb161c9d259c4f8ab6e1edf986c88c622b19443",
    remote = "https://github.com/jwmcglynn/skia",
)

load("@skia//bazel:deps.bzl", "git_repos_from_deps")

git_repos_from_deps()

# Skia dependencies

http_archive(
    name = "rules_python",
    sha256 = "5868e73107a8e85d8f323806e60cad7283f34b32163ea6ff1020cf27abef6036",
    strip_prefix = "rules_python-0.25.0",
    url = "https://github.com/bazelbuild/rules_python/releases/download/0.25.0/rules_python-0.25.0.tar.gz",
)

load("@rules_python//python:repositories.bzl", "py_repositories")

py_repositories()

http_archive(
    name = "io_bazel_rules_go",
    sha256 = "278b7ff5a826f3dc10f04feaf0b70d48b68748ccd512d7f98bf442077f043fe3",
    urls = [
        "https://mirror.bazel.build/github.com/bazelbuild/rules_go/releases/download/v0.41.0/rules_go-v0.41.0.zip",
        "https://github.com/bazelbuild/rules_go/releases/download/v0.41.0/rules_go-v0.41.0.zip",
    ],
)

load("@io_bazel_rules_go//go:deps.bzl", "go_register_toolchains", "go_rules_dependencies")

go_rules_dependencies()

go_register_toolchains(version = "1.20.2")

# resvg test suite

new_git_repository(
    name = "resvg-test-suite",
    build_file = "@//third_party:BUILD.resvg-test-suite",
    commit = "682a9c8da8c580ad59cba0ef8cb8a8fd5534022f",
    remote = "https://github.com/RazrFalcon/resvg-test-suite.git",
)

# wasm build (emscripten)

http_archive(
    name = "emsdk",
    sha256 = "6615d8a4b98f4fc1e6107878b7f65828b2c07649d8b0dc1e661b83bb6ddf7ca3",
    strip_prefix = "emsdk-d42c740aa51b961d6e18b649754d2cae8807a277/bazel",
    url = "https://github.com/emscripten-core/emsdk/archive/d42c740aa51b961d6e18b649754d2cae8807a277.tar.gz",
)

load("@emsdk//:deps.bzl", emsdk_deps = "deps")

emsdk_deps()

load("@emsdk//:emscripten_deps.bzl", emsdk_emscripten_deps = "emscripten_deps")

# Version should match "latest-arm64-linux" in https://github.com/emscripten-core/emsdk/blob/main/emscripten-releases-tags.json
emsdk_emscripten_deps(emscripten_version = "3.1.33")

load("@emsdk//:toolchains.bzl", "register_emscripten_toolchains")

register_emscripten_toolchains()

# scip-clang for Sourcegraph

load("//third_party/scip-clang:defs.bzl", "gen_scip_clang")

gen_scip_clang()

# Python pip dependencies (for tools)

load("@rules_python//python:pip.bzl", "pip_parse")

pip_parse(
    name = "pip_deps",
    requirements_lock = "//tools/python:requirements.txt",
)

load("@pip_deps//:requirements.bzl", "install_deps")

install_deps()

# hdoc

new_local_repository(
    name = "hdoc",
    build_file = "third_party/BUILD.hdoc",
    path = "third_party/hdoc",
    workspace_file = "third_party/WORKSPACE.hdoc",
)

load("@//third_party:hdoc.bzl", "hdoc_dependencies")

hdoc_dependencies()
