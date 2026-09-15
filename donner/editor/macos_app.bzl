"""macOS application assembly from the editor's declared runtime closure."""

def _packaged_editor_transition_impl(settings, attr):
    return {"//build_defs:enable_tracy": False}

_packaged_editor_transition = transition(
    implementation = _packaged_editor_transition_impl,
    inputs = [],
    outputs = ["//build_defs:enable_tracy"],
)

def _macos_editor_app_impl(ctx):
    editor = ctx.attr.editor
    if type(editor) == "list":
        if len(editor) != 1:
            fail("Expected one packaged editor configuration")
        editor = editor[0]
    info = editor[DefaultInfo]
    binary = info.files_to_run.executable
    alias = "_donner_transitioned/{}/{}".format(editor.label.package, editor.label.name)
    for entry in info.default_runfiles.root_symlinks.to_list():
        if entry.path == alias:
            binary = entry.target_file
    if binary == None:
        fail("editor must expose an executable")
    archive = ctx.actions.declare_file("Donner SVG Editor.app.zip")
    checksum = ctx.actions.declare_file("Donner SVG Editor.app.zip.sha256")
    args = ctx.actions.args()
    args.add("--editor", binary)
    args.add("--renderer", ctx.executable.renderer)
    args.add("--icon", ctx.file.icon)
    args.add("--agents", ctx.file.agents)
    args.add("--skill", ctx.file.skill)
    args.add("--vectorization", ctx.file.vectorization)
    args.add("--adapter", ctx.file.adapter)
    args.add("--output", archive)
    args.add("--checksum", checksum)
    args.add("--status-file", ctx.info_file)
    args.add("--module", ctx.file.module)
    files = info.default_runfiles.files.to_list()
    for file in files:
        if file.basename.endswith(".dylib"):
            args.add("--runtime", file)
    inputs = depset([binary, ctx.file.icon, ctx.file.agents, ctx.file.skill, ctx.file.vectorization, ctx.file.adapter, ctx.file.module, ctx.info_file], transitive = [info.default_runfiles.files])
    ctx.actions.run(
        executable = ctx.attr.packager[DefaultInfo].files_to_run,
        arguments = [args],
        inputs = inputs,
        tools = [ctx.attr.renderer[DefaultInfo].files_to_run],
        outputs = [archive, checksum],
        mnemonic = "MacOSEditorBundle",
        progress_message = "Packaging Donner SVG Editor.app",
        use_default_shell_env = True,
        execution_requirements = {"no-remote": "1"},
    )
    return [DefaultInfo(files = depset([archive, checksum]))]

macos_editor_app = rule(
    implementation = _macos_editor_app_impl,
    attrs = {
        "editor": attr.label(mandatory = True, cfg = _packaged_editor_transition),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
        "renderer": attr.label(executable = True, cfg = "exec", mandatory = True),
        "packager": attr.label(executable = True, cfg = "exec", mandatory = True),
        "icon": attr.label(allow_single_file = [".svg"], mandatory = True),
        "agents": attr.label(allow_single_file = True, mandatory = True),
        "skill": attr.label(allow_single_file = True, mandatory = True),
        "vectorization": attr.label(allow_single_file = True, mandatory = True),
        "adapter": attr.label(allow_single_file = True, mandatory = True),
        "module": attr.label(allow_single_file = True, mandatory = True),
    },
)
