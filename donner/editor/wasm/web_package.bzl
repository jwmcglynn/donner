def _web_package_impl(ctx):
    output_dir = ctx.actions.declare_directory(ctx.attr.out if ctx.attr.out else ctx.attr.name)

    args = ctx.actions.args()
    args.add("--output", output_dir.path)
    for input_file in ctx.files.srcs + ctx.files.wasm_deps:
        args.add("--file", input_file.path)
    for tree in ctx.files.asset_trees:
        args.add("--tree", tree.path)

    ctx.actions.run(
        executable = ctx.executable._packager,
        arguments = [args],
        inputs = ctx.files.srcs + ctx.files.wasm_deps + ctx.files.asset_trees,
        outputs = [output_dir],
        mnemonic = "EditorWebPackage",
    )

    return [DefaultInfo(
        files = depset([output_dir]),
    )]

web_package = rule(
    implementation = _web_package_impl,
    attrs = {
        "srcs": attr.label_list(
            allow_files = [".css", ".html", ".js", ".svg", ".woff2", ".json", ".txt"],
        ),
        "out": attr.string(),
        "wasm_deps": attr.label_list(),
        "asset_trees": attr.label_list(),
        "_packager": attr.label(
            default = Label("//donner/editor/wasm:package_web_files"),
            executable = True,
            cfg = "exec",
        ),
    },
)
