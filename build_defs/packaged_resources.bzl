"""Generate metadata-only C++ resources and complete content-addressed data trees."""

load("@rules_cc//cc:defs.bzl", "cc_library")

def _generate_impl(ctx):
    header = ctx.actions.declare_file(ctx.attr.header)
    assets = ctx.actions.declare_directory(ctx.label.name + ".assets")
    request = ctx.actions.declare_file(ctx.label.name + ".request.json")
    ctx.actions.write(request, json.encode({
        "group": ctx.attr.group,
        "runfilesRoot": "_main/" + ctx.label.package + "/" + assets.basename,
        "resources": [
            {"id": key, "source": target.files.to_list()[0].path}
            for key, target in zip(ctx.attr.ids, ctx.attr.srcs)
        ],
    }))
    ctx.actions.run(
        executable = ctx.executable._generator,
        inputs = depset(ctx.files.srcs + [request]),
        outputs = [header, assets],
        arguments = ["--request", request.path, "--header", header.path, "--output", assets.path],
        mnemonic = "PackagedResources",
    )
    return [
        DefaultInfo(files = depset([header]), runfiles = ctx.runfiles(files = [assets])),
        OutputGroupInfo(resources = depset([assets])),
    ]

packaged_resources_generate = rule(
    implementation = _generate_impl,
    attrs = {
        "ids": attr.string_list(mandatory = True),
        "srcs": attr.label_list(allow_files = True, mandatory = True),
        "header": attr.string(mandatory = True),
        "header_output": attr.string(mandatory = True),
        "resource_name": attr.string(mandatory = True),
        "group": attr.string(mandatory = True),
        "_generator": attr.label(default = "//build_defs:packaged_resources_generator", executable = True, cfg = "exec"),
    },
)

def packaged_resources(name, resources, header_output, group = "chrome", **kwargs):
    """Keep stable resource names while moving all payload bytes into package data."""
    filename = header_output.split("/")[-1]
    prefix = "/".join(header_output.split("/")[:-1])
    packaged_resources_generate(
        name = name + "_resource_gen",
        ids = sorted(resources.keys()),
        srcs = [resources[key] for key in sorted(resources.keys())],
        header = filename,
        header_output = header_output,
        resource_name = name,
        group = group,
        target_compatible_with = kwargs.get("target_compatible_with", []),
    )
    native.filegroup(
        name = name + "_resource_files",
        srcs = [":" + name + "_resource_gen"],
        output_group = "resources",
        target_compatible_with = kwargs.get("target_compatible_with", []),
        visibility = ["//visibility:public"],
    )
    cc_library(
        name = name,
        hdrs = [":" + name + "_resource_gen"],
        data = [":" + name + "_resource_files"],
        include_prefix = prefix,
        deps = ["//donner/base:packaged_resource"],
        **kwargs
    )

def _bundle_impl(ctx):
    output = ctx.actions.declare_directory(ctx.attr.output_name or ctx.label.name + ".assets")
    inputs = ctx.files.resources
    ctx.actions.run(
        executable = ctx.executable._generator,
        inputs = inputs,
        outputs = [output],
        arguments = ["--output", output.path, "--merge"] + [file.path for file in inputs],
        mnemonic = "PackagedResourceBundle",
    )
    return [DefaultInfo(files = depset([output]), runfiles = ctx.runfiles(files = [output]))]

packaged_resource_bundle = rule(
    implementation = _bundle_impl,
    attrs = {
        "resources": attr.label_list(allow_files = True, mandatory = True),
        "output_name": attr.string(),
        "_generator": attr.label(default = "//build_defs:packaged_resources_generator", executable = True, cfg = "exec"),
    },
)
