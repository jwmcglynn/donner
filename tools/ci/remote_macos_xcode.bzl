"""A static Xcode configuration for building for macOS on a remote macOS executor.

Bazel normally discovers the installed Xcodes on the client. A client that is not a Mac finds
none, and its discovered configuration is empty, so a build for macOS that runs every action on a
remote macOS executor selects this configuration with `--xcode_version_config` instead
(`--config=remote-macos` in .bazelrc). It describes the Xcode the executor must have installed at
the standard path; it does not discover or check what the executor has.

The approved Xcode tuples come from tools/ci_macos_xcode.txt, the single policy that the CI
selector (tools/ci_select_macos_xcode.sh) reads. Starlark cannot read that file without a
repository rule, so PINNED_XCODES restates it, and //tools:remote_macos_config_tests fails
whenever the two differ, including in order.
"""

load("@build_bazel_apple_support//xcode:xcode_config.bzl", "xcode_config")
load("@build_bazel_apple_support//xcode:xcode_version.bzl", "xcode_version")

# The approved (version, build) pairs of tools/ci_macos_xcode.txt, in its preference order.
PINNED_XCODES = [
    ("27.1", "27A9275"),
    ("27.2", "27B52"),
]

# SDK versions reported by each pinned Xcode build, recorded only after they were read on a
# machine with that Xcode installed (`xcodebuild -showsdks`). Apple toolchain actions carry the
# selected SDK version in their environment and find the SDK directory by it, so a wrong value
# fails the build or changes its action keys. A pinned build without an entry here is left out of
# the static configuration rather than given guessed values.
CONFIRMED_SDK_VERSIONS = {
    "27B52": {
        "ios": "27.2",
        "macos": "27.2",
        "tvos": "27.2",
        "visionos": "27.2",
        "watchos": "27.2",
    },
}

def _full_version(version, build):
    """`27.2`, `27B52` -> `27.2.0.27B52`, the form Bazel's Xcode discovery records."""
    parts = version.split(".")
    return ".".join(parts + ["0"] * (3 - len(parts)) + [build])

def remote_macos_xcode_config(name, visibility = None):
    """Defines `name`, an xcode_config holding each pinned Xcode with confirmed SDK versions.

    The default is the most preferred such Xcode, which a build gets without `--xcode_version`.
    Each Xcode answers to its version, its three-part version and its build. The major version
    alone is not an alias: two pinned Xcodes can share it.

    Every target is `manual`. They are reached only through `--xcode_version_config`, and an
    xcode_config fails analysis when `--xcode_version` names a version it does not hold, so in a
    wildcard build with another Xcode (a macOS CI lane on a pinned Xcode without confirmed SDK
    versions here) they would fail the whole build.

    Args:
      name: The xcode_config target name. Each Xcode is `<name>_<version>` with `_` for `.`.
      visibility: Visibility of the xcode_config and its versions.
    """
    versions = []
    for version, build in PINNED_XCODES:
        sdks = CONFIRMED_SDK_VERSIONS.get(build)
        if not sdks:
            continue
        target = "%s_%s" % (name, version.replace(".", "_"))
        aliases = [version]
        three_part = _full_version(version, build).rsplit(".", 1)[0]
        if three_part != version:
            aliases.append(three_part)
        aliases.append(build)
        xcode_version(
            name = target,
            version = _full_version(version, build),
            aliases = aliases,
            default_ios_sdk_version = sdks["ios"],
            default_macos_sdk_version = sdks["macos"],
            default_tvos_sdk_version = sdks["tvos"],
            default_visionos_sdk_version = sdks["visionos"],
            default_watchos_sdk_version = sdks["watchos"],
            tags = ["manual"],
            visibility = visibility,
        )
        versions.append(":" + target)
    if not versions:
        fail("No pinned Xcode in tools/ci_macos_xcode.txt has confirmed SDK versions.")
    xcode_config(
        name = name,
        default = versions[0],
        versions = versions,
        tags = ["manual"],
        visibility = visibility,
    )
