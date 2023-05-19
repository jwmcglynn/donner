def _web_package_impl(ctx):
    output_dir = ctx.actions.declare_directory(ctx.attr.out if ctx.attr.out else ctx.attr.name)

    commands = ["mkdir -p {}".format(output_dir.path)]

    # Copy files to the output directory
    for input_file in ctx.files.srcs:
        output_path = output_dir.path + "/" + input_file.basename
        commands.append("cp {} {}".format(input_file.path, output_path))

    # Copy wasm files to the output directory
    for wasm_dep in ctx.files.wasm_deps:
        output_path = output_dir.path + "/" + wasm_dep.basename
        commands.append("cp {} {}".format(wasm_dep.path, output_path))

    ctx.actions.run_shell(
        inputs = ctx.files.srcs + ctx.files.wasm_deps,
        outputs = [output_dir],
        command = " && ".join(commands),
    )

    return [DefaultInfo(
        files = depset([output_dir]),
    )]

web_package = rule(
    implementation = _web_package_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = [".html", ".js", ".css"]),
        "wasm_deps": attr.label_list(),
        "out": attr.string(),
    },
)
