"""
Donner Bazel workspace rules.
"""

workspace(name = "donner")

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository", "new_git_repository")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

##
## Bazel and IntelliSense
##

## Developer tools

git_repository(
    name = "bazel_clang_tidy",
    commit = "674fac7640ae0469b7a58017018cb1497c26e2bf",
    remote = "https://github.com/erenon/bazel_clang_tidy.git",
)

##
## Third-party dependencies.
##

new_local_repository(
    name = "css-parsing-tests",
    build_file = "//third_party:BUILD.css-parsing-tests",
    path = "third_party/css-parsing-tests",
)

new_local_repository(
    name = "rapidxml_ns",
    build_file = "//third_party:BUILD.rapidxml_ns",
    path = "third_party/rapidxml_ns",
)

git_repository(
    name = "entt",
    commit = "a03b88e0eb679ca0fbb79d9c54293e772b4f4a93",
    remote = "https://github.com/skypjack/entt",
)

new_local_repository(
    name = "frozen",
    build_file = "//third_party:BUILD.frozen",
    path = "third_party/frozen",
)

git_repository(
    name = "range-v3",
    branch = "master",
    remote = "https://github.com/ericniebler/range-v3",
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
    sha256 = "d71d2c67e0bce986e1c5a7731b4693226867c45bfe0b7c5e0067228a536fc580",
    strip_prefix = "rules_python-0.29.0",
    url = "https://github.com/bazelbuild/rules_python/releases/download/0.29.0/rules_python-0.29.0.tar.gz",
)

load("@rules_python//python:repositories.bzl", "py_repositories")

py_repositories()

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
    sha256 = "9adce979c99b7d172c5542403150729e58dc311937aa9d245438447e26cae908",
    strip_prefix = "emsdk-2aa74907151b2caa9da865fd0d36436fdce792f0/bazel",
    url = "https://github.com/emscripten-core/emsdk/archive/2aa74907151b2caa9da865fd0d36436fdce792f0.tar.gz",
)

load("@emsdk//:deps.bzl", emsdk_deps = "deps")

emsdk_deps()

load("@emsdk//:emscripten_deps.bzl", emsdk_emscripten_deps = "emscripten_deps")

# Version should match "latest-arm64-linux" in https://github.com/emscripten-core/emsdk/blob/main/emscripten-releases-tags.json
emsdk_emscripten_deps(emscripten_version = "3.1.47")

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
    build_file = "//third_party:BUILD.hdoc",
    path = "third_party/hdoc",
    workspace_file = "//third_party:WORKSPACE.hdoc",
)

load("@//third_party:hdoc.bzl", "hdoc_dependencies")

hdoc_dependencies()
