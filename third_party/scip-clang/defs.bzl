load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_file")

VERSION = "v0.0.6"

SCIP_CLANG_VERSIONS = {
    "v0.0.6": {
        "darwin": "afec5faf75a0fd7bf41e8b12d28603adef2f17305bf90460ee2dbbf3f72d395d",
        "linux": "1fad65214b18b2f6cb37c00ffab8f1958b575f11b9e48ea6911c72dfa5583501",
    },
}

def gen_scip_clang(name = "gen_scip_clang", version = VERSION):
    release_url = "https://github.com/sourcegraph/scip-clang/releases/download/" + version

    for platform, sha256 in SCIP_CLANG_VERSIONS[version].items():
        http_file(
            name = "scip-clang-" + platform + "-repo",
            downloaded_file_path = "scip-clang-" + platform,
            sha256 = sha256,
            urls = [release_url + "/scip-clang-x86_64-" + platform],
            executable = True,
        )