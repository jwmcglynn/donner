def _web_package_impl(ctx):
    output_dir = ctx.actions.declare_directory(ctx.attr.out if ctx.attr.out else ctx.attr.name)
    outs = []

    # Create the output directory if it doesn't exist
    ctx.actions.run_shell(
        inputs = [],
        outputs = [output_dir],
        command = "mkdir -p {}".format(output_dir.path),
    )

    # Copy files to the output directory
    for input_file in ctx.files.srcs:
        output_path = output_dir.short_path + "/" + input_file.basename
        output_file = ctx.actions.declare_file(output_path)
        outs.append(output_file)

        ctx.actions.run_shell(
            inputs = [input_file],
            outputs = [output_file],
            command = "cp {} {}".format(input_file.path, output_file.path),
        )

    # Copy wasm files to the output directory
    for wasm_dep in ctx.files.wasm_deps:
        output_path = output_dir.short_path + "/" + wasm_dep.basename
        output_file = ctx.actions.declare_file(output_path)
        outs.append(output_file)

        ctx.actions.run_shell(
            inputs = [wasm_dep],
            outputs = [output_file],
            command = "cp {} {}".format(wasm_dep.path, output_file.path),
        )

    return [DefaultInfo(files = depset([output_dir], transitive = [depset(outs)]))]

web_package = rule(
    implementation = _web_package_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = [".html", ".js", ".css"]),
        "wasm_deps": attr.label_list(),
        "out": attr.string(),
    },
)
