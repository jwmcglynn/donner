"""Generate editor identity with a cache-keyed release commit or a local developer fallback."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

def _editor_build_info_impl(ctx):
    commit = ctx.attr._release_commit[BuildSettingInfo].value
    if commit and (len(commit) != 40 or any([c not in "0123456789abcdef" for c in commit.elems()])):
        fail("release_source_commit must be empty or a full lowercase Git SHA")
    ctx.actions.run_shell(
        inputs = [ctx.file.module, ctx.file.script],
        outputs = [ctx.outputs.out],
        arguments = [ctx.file.script.path, ctx.file.module.path, ctx.outputs.out.path, commit],
        command = '/bin/bash "$1" "$2" "$3" "$4"',
        env = {"PATH": "/usr/bin:/bin"},
        mnemonic = "DonnerEditorBuildInfo",
        progress_message = "Writing release editor build identity" if commit else "Executing genrule //donner/editor:editor_build_info_txt",
        execution_requirements = {} if commit else {"local": "1"},
    )
    return [DefaultInfo(files = depset([ctx.outputs.out]))]

donner_editor_build_info = rule(
    implementation = _editor_build_info_impl,
    doc = "Generate version/commit metadata; an explicit release commit is an action-key input.",
    attrs = {
        "module": attr.label(mandatory = True, allow_single_file = True),
        "script": attr.label(mandatory = True, allow_single_file = True),
        "out": attr.output(mandatory = True),
        "_release_commit": attr.label(default = "//build_defs:release_source_commit"),
    },
)
