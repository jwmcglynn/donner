load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

local_repository(
    name = "gtest",
    path = "third_party/gtest",
)

local_repository(
    name = "absl",
    path = "third_party/absl",
)

git_repository(
    name = "entt",
    remote = "https://github.com/skypjack/entt.git",
    tag = "v3.4.0",
)
