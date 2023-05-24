"""
Generates an http_file rule to download the latest scip-clang binary, which is used for
Sourcegraph code intelligence.

See README.md for more information.

To update to a new version, find latest release at
https://github.com/sourcegraph/scip-clang/releases/

And then run: bazel run //third_party/scip-clang:fetch_new_version -- <tag>
"""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_file")

VERSION = "v0.1.3"

SCIP_CLANG_VERSIONS = {
    "v0.1.3": {
        "darwin": "97fe20f0d38617a5579e78e33ef2cb05b06a12f494a7643626a1fe1261e7a518",
        "linux": "26176b41bc94be1c938331ac2702d133238d6a5a3869cf9f0980fd8eebacd70d",
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
