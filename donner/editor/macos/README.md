# Donner SVG Editor for macOS

Build the app archive from a committed checkout:

```sh
bazel build -c opt --workspace_status_command=tools/editor_workspace_status.sh \
  //donner/editor:macos_app
```

The outputs are `bazel-bin/donner/editor/Donner SVG Editor.app.zip` and its SHA-256
checksum. The package includes the native editor, relocated runtime libraries, an icon
rendered from `editor_icon.svg` with Donner, and agent guidance. Assembly signs the app
ad hoc and verifies the signature and runtime paths before creating the archive.
These development signatures are not Developer ID signing or notarization.
The app target disables Tracy profiling, including its developer network listener.

The macOS Editor App workflow produces the same archive in CI. The release workflow
builds it once, verifies its checksum, attests it, and uploads those exact bytes with the
other release assets.

## Agent entry point

After installing the app, an agent can start with:

> Edit an SVG using Donner; see
> `/Applications/Donner SVG Editor.app/Contents/Resources/AGENTS.md` for instructions.

Instructions live inside `Contents/Resources` because macOS signing rejects extra files
at the app bundle root. All links are relative, so the app can be installed elsewhere.

Use the editor's `--help` to discover launch options. Collaboration is opt-in through
`--control-socket`; the bundled `agent/editor_control_wrapper.py` bridges that private
socket to MCP over stdio. The adapter currently requires an available Python 3 runtime.
The editor itself does not depend on Python.

The app owns the live document. Agent edits use the editor's DOM tools and shared undo
history; agents should not overwrite the SVG file behind an active editing session.
