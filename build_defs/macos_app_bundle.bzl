"""Wrap a transitioned native editor binary in a macOS application bundle."""

load("//build_defs:rules.bzl", "DonnerTransitionedBinaryInfo")

def _macos_app_bundle_impl(ctx):
    binary = ctx.attr.binary[DonnerTransitionedBinaryInfo].binary
    app = ctx.actions.declare_directory("Donner.app")
    launcher = ctx.actions.declare_file(ctx.label.name)

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

    arguments = [app.path, binary.path, ctx.file.plist.path, ctx.file.icon.path]
    for dylib in dylibs:
        arguments.extend([dylib.path, dylib.basename])
    ctx.actions.run_shell(
        inputs = depset([binary, ctx.file.plist, ctx.file.icon] + dylibs),
        outputs = [app],
        arguments = arguments,
        command = """set -eu
app="$1"; binary="$2"; plist="$3"; icon="$4"
shift 4
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" "$app/Contents/Frameworks"
cp "$binary" "$app/Contents/MacOS/Donner"
chmod u+w "$app/Contents/MacOS/Donner"
chmod 755 "$app/Contents/MacOS/Donner"
cp "$plist" "$app/Contents/Info.plist"
cp "$icon" "$app/Contents/Resources/Donner.icns"
while [ "$#" -gt 0 ]; do
  cp "$1" "$app/Contents/Frameworks/$2"
  chmod u+w "$app/Contents/Frameworks/$2"
  /usr/bin/codesign --force --sign - --timestamp=none "$app/Contents/Frameworks/$2"
  shift 2
done
/usr/bin/codesign --force --sign - --timestamp=none "$app/Contents/MacOS/Donner"
/usr/bin/codesign --force --sign - --timestamp=none "$app"
""",
        mnemonic = "DonnerMacosAppBundle",
        progress_message = "Bundling Donner.app",
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

    return [DefaultInfo(
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
        "plist": attr.label(mandatory = True, allow_single_file = [".plist"]),
    },
)
