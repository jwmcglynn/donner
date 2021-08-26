load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository", "new_git_repository")

git_repository(
    name = "com_google_gtest",
    remote = "https://github.com/google/googletest",
    tag = "release-1.10.0",
)

# Use absl at head.
git_repository(
    name = "com_google_absl",
    branch = "master",
    remote = "https://github.com/abseil/abseil-cpp",
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
    commit = "9d72ffb9fe0ce4a15bab3729b52fcb0adace4d7f",
    remote = "https://github.com/skypjack/entt.git",
)

new_local_repository(
    name = "frozen",
    build_file = "third_party/BUILD.frozen",
    path = "third_party/frozen",
)

new_git_repository(
    name = "nlohmann_json",
    build_file = "@//third_party:BUILD.nlohmann_json",
    remote = "https://github.com/nlohmann/json.git",
    tag = "v3.9.1",
)

git_repository(
    name = "io_bazel_rules_rust",
    commit = "7cf9a3fc467f547b878f7d4065fcd8737da38803",
    remote = "https://github.com/jwmcglynn/rules_rust.git",
)

git_repository(
    name = "bazel_skylib",
    commit = "560d7b2359aecb066d81041cb532b82d7354561b",
    remote = "https://github.com/bazelbuild/bazel-skylib.git",
)

git_repository(
    name = "stb",
    branch = "master",
    init_submodules = True,
    remote = "https://github.com/nitronoid/rules_stb",
)

load("@io_bazel_rules_rust//rust:repositories.bzl", "rust_repositories")

rust_repositories(
    cargo_version = "0.45.0",
    version = "1.44.0",
)

load("@io_bazel_rules_rust//:workspace.bzl", "bazel_version")

bazel_version(name = "bazel_version")

load("//third_party/pathfinder:crates.bzl", "raze_fetch_remote_crates")

raze_fetch_remote_crates()
