"""Wrap a transitioned native editor binary in a macOS application bundle."""

load("//build_defs:rules.bzl", "DonnerTransitionedBinaryInfo")

def _macos_app_bundle_impl(ctx):
    binary = ctx.attr.binary[DonnerTransitionedBinaryInfo].binary
    app = ctx.actions.declare_directory("Donner.app")
    launcher = ctx.actions.declare_file(ctx.label.name)
    test_archive = ctx.actions.declare_file("Donner.app.test-data.tar")

    runfiles = ctx.attr.binary[DefaultInfo].default_runfiles.files.to_list()
    dylibs = []
    names = {}
    for file in runfiles:
        if not file.basename.endswith(".dylib"):
            continue
        if file.basename in names and names[file.basename] != file.path:
            fail("macOS app has conflicting dylibs named {}".format(file.basename))
        names[file.basename] = file.path
        dylibs.append(file)

    arguments = [app.path, binary.path, ctx.file.plist.path, ctx.file.icon.path, test_archive.path, ctx.file.resources.path]
    for dylib in dylibs:
        arguments.extend([dylib.path, dylib.basename])
    ctx.actions.run_shell(
        inputs = depset([binary, ctx.file.plist, ctx.file.icon, ctx.file.resources] + dylibs),
        outputs = [app, test_archive],
        arguments = arguments,
        command = """set -eu
umask 022
app="$1"; binary="$2"; plist="$3"; icon="$4"; test_archive="$5"; resources="$6"
shift 6
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" "$app/Contents/Frameworks"
cp "$binary" "$app/Contents/MacOS/Donner"
chmod u+w "$app/Contents/MacOS/Donner"
chmod 755 "$app/Contents/MacOS/Donner"
cp "$plist" "$app/Contents/Info.plist"
cp "$icon" "$app/Contents/Resources/Donner.icns"
mkdir -p "$app/Contents/Resources/donner-assets"
cp -R "$resources/." "$app/Contents/Resources/donner-assets/"
find "$app/Contents/Resources/donner-assets" -type d -exec chmod 755 {} +
find "$app/Contents/Resources/donner-assets" -type f -exec chmod 644 {} +
chmod 644 "$app/Contents/Info.plist" "$app/Contents/Resources/Donner.icns"
while [ "$#" -gt 0 ]; do
  cp "$1" "$app/Contents/Frameworks/$2"
  chmod u+w "$app/Contents/Frameworks/$2"
  /usr/bin/codesign --force --sign - --timestamp=none "$app/Contents/Frameworks/$2"
  shift 2
done
/usr/bin/codesign --force --sign - --timestamp=none "$app/Contents/MacOS/Donner"
/usr/bin/codesign --force --sign - --timestamp=none "$app"
# Capture the signed producer files before TreeArtifact runfiles introduce transport links.
COPYFILE_DISABLE=1 /usr/bin/tar --format=ustar --uid 0 --gid 0 --uname root --gname wheel -cf "$test_archive" -C "$app" Contents
""",
        mnemonic = "DonnerMacosAppBundle",
        progress_message = "Bundling Donner.app",
        # /usr/bin/codesign comes from the host macOS, not an action input, so its version is
        # not in the action key. Runners on different macOS builds share one remote cache
        # partition (the pinned Xcode), so the signed bundle must never come from that cache.
        execution_requirements = {"no-remote-cache": "1"},
    )

    ctx.actions.write(
        launcher,
        """#!/usr/bin/env bash
set -euo pipefail
bundle="$(cd "$(dirname "$0")" && pwd)/Donner.app"
if [[ ! -d "$bundle" ]]; then
  bundle="${{RUNFILES_DIR:-$0.runfiles}}/{workspace}/{package}/Donner.app"
fi
exec "$bundle/Contents/MacOS/Donner" "$@"
""".format(workspace = ctx.workspace_name, package = ctx.label.package),
        is_executable = True,
    )

    return [OutputGroupInfo(native_test_archive = depset([test_archive])), DefaultInfo(
        executable = launcher,
        files = depset([launcher, app]),
        runfiles = ctx.runfiles(files = [app]),
    )]

donner_macos_app_bundle = rule(
    implementation = _macos_app_bundle_impl,
    executable = True,
    attrs = {
        "binary": attr.label(mandatory = True, providers = [DonnerTransitionedBinaryInfo]),
        "icon": attr.label(mandatory = True, allow_single_file = [".icns"]),
        "resources": attr.label(mandatory = True, allow_single_file = True),
        "plist": attr.label(mandatory = True, allow_single_file = [".plist"]),
    },
)
