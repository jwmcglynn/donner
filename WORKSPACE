"""
Donner Bazel workspace rules.
"""

workspace(name = "donner")

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository", "new_git_repository")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

##
## Toolchain
##

BAZEL_TOOLCHAIN_TAG = "0.8.2"

BAZEL_TOOLCHAIN_SHA = "0fc3a2b0c9c929920f4bed8f2b446a8274cad41f5ee823fd3faa0d7641f20db0"

http_archive(
    name = "com_grail_bazel_toolchain",
    canonical_id = BAZEL_TOOLCHAIN_TAG,
    sha256 = BAZEL_TOOLCHAIN_SHA,
    strip_prefix = "bazel-toolchain-{tag}".format(tag = BAZEL_TOOLCHAIN_TAG),
    url = "https://github.com/grailbio/bazel-toolchain/archive/refs/tags/{tag}.tar.gz".format(tag = BAZEL_TOOLCHAIN_TAG),
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
    commit = "e085566bf35e020402a2e32258360b16446fbad8",
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
    branch = "main",
    remote = "https://github.com/google/googletest",
)

# Use absl at head.
git_repository(
    name = "com_google_absl",
    branch = "master",
    remote = "https://github.com/abseil/abseil-cpp",
)

# Note this must use a commit from the `abseil` branch of the RE2 project.
# https://github.com/google/re2/tree/abseil
http_archive(
    name = "com_googlesource_code_re2",
    sha256 = "0a890c2aa0bb05b2ce906a15efb520d0f5ad4c7d37b8db959c43772802991887",
    strip_prefix = "re2-a427f10b9fb4622dd6d8643032600aa1b50fbd12",
    urls = ["https://github.com/google/re2/archive/a427f10b9fb4622dd6d8643032600aa1b50fbd12.zip"],  # 2022-06-09
)

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

# Skia dependencies

http_archive(
    name = "rules_python",
    sha256 = "a644da969b6824cc87f8fe7b18101a8a6c57da5db39caa6566ec6109f37d2141",
    strip_prefix = "rules_python-0.20.0",
    url = "https://github.com/bazelbuild/rules_python/releases/download/0.20.0/rules_python-0.20.0.tar.gz",
)

load("@rules_python//python:repositories.bzl", "py_repositories")

py_repositories()

http_archive(
    name = "io_bazel_rules_go",
    sha256 = "6b65cb7917b4d1709f9410ffe00ecf3e160edf674b78c54a894471320862184f",
    urls = [
        "https://mirror.bazel.build/github.com/bazelbuild/rules_go/releases/download/v0.39.0/rules_go-v0.39.0.zip",
        "https://github.com/bazelbuild/rules_go/releases/download/v0.39.0/rules_go-v0.39.0.zip",
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
    sha256 = "c60665436302edf92a8c48b4decf1acea0a423622e10bc8caa210c01fd1f34be",
    strip_prefix = "emsdk-da9699832b5df4e123403490e499c87000c22654/bazel",
    url = "https://github.com/emscripten-core/emsdk/archive/da9699832b5df4e123403490e499c87000c22654.tar.gz",
)

load("@emsdk//:deps.bzl", emsdk_deps = "deps")

emsdk_deps()

load("@emsdk//:emscripten_deps.bzl", emsdk_emscripten_deps = "emscripten_deps")

emsdk_emscripten_deps(emscripten_version = "3.1.34")

load("@emsdk//:toolchains.bzl", "register_emscripten_toolchains")

register_emscripten_toolchains()
