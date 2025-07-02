"""
Embed resource files into a C++ source file.
"""

load("@rules_cc//cc:defs.bzl", "cc_library")

def _sanitize_filename(filename):
    """Returns a sanitized version of the given filename.

    This is used to generate C++ symbols from filenames, so that they can be used as identifiers.
    """
    return "".join(
        [ch if ch.isalnum() else "_" for ch in filename.elems()],
    )

def _asset_to_cpp_impl(ctx):
    """Run `xxd -i` to convert the input file to a C array, used to embed files in the binary.

    For the given filename, an array will be defined using the provided variable_name parameter,
    with all non-alphanumeric characters replaced with underscore.

    For example, if variable_name is "my_resource", the following constants will be defined in the output
    file:
    - `my_resource`
    - `my_resource_len`

    To use them, add the following `extern` declarations to your code:

    ```cpp
    extern uint8_t      my_resource[];
    extern unsigned int my_resource_len;
    ```
    """

    variable_name = ctx.attr.variable_name

    ctx.actions.run(
        inputs = [ctx.file.src],
        outputs = [ctx.outputs.out],
        executable = ctx.executable._xxd,
        arguments = [
            "-i",
            "-n",
            variable_name,
            ctx.file.src.path,
            ctx.outputs.out.path,
        ],
    )

asset_to_cpp = rule(
    implementation = _asset_to_cpp_impl,
    attrs = {
        "src": attr.label(mandatory = True, allow_single_file = True, doc = "The asset to embed."),
        "out": attr.output(mandatory = True, doc = "The output file."),
        "variable_name": attr.string(mandatory = True, doc = "The name of the variable to define in the output file."),
        "_xxd": attr.label(
            executable = True,
            default = "//third_party/xxd",
            cfg = "exec",
        ),
    },
)

def _embed_resources_generate_header_impl(ctx):
    """Implementation for generating the C++ header that exposes embedded resources.

    This implementation generates a C++ header file that declares extern symbols for embedded
    resources and provides convenient std::span accessors in the donner::embedded namespace.

    For each variable name provided in variable_names, it generates a
    `const std::span<const unsigned char>` in the `donner::embedded` namespace that wraps the array.
    """
    variable_names = ctx.attr.variable_names

    header_content = (
        "#pragma once\n\n" +
        "#include <span>\n\n"
    )
    for variable_name in variable_names:
        header_content += (
            "extern unsigned char __donner_embedded_{var}[];\n" +
            "extern unsigned int __donner_embedded_{var}_len;\n\n" +
            "namespace donner::embedded {{\n" +
            "inline const std::span<const unsigned char> {var}(__donner_embedded_{var}, __donner_embedded_{var}_len);\n" +
            "}}  // namespace donner::embedded\n\n"
        ).format(var = variable_name)

    ctx.actions.write(
        output = ctx.outputs.out,
        content = header_content,
    )

embed_resources_generate_header = rule(
    implementation = _embed_resources_generate_header_impl,
    attrs = {
        "variable_names": attr.string_list(mandatory = True, doc = "Names of variables to expose."),
        "out": attr.output(mandatory = True, doc = "Header file to generate."),
    },
)

def _save_repro_dict_impl(ctx):
    """Implementation for rule that outputs the header_output and resources parameters as JSON."""
    repro_json = {
        "header_output": ctx.attr.header_output,
        "resources": ctx.attr.resources,
    }

    ctx.actions.write(
        output = ctx.outputs.out,
        content = json.encode(repro_json),
    )

_save_repro_dict = rule(
    implementation = _save_repro_dict_impl,
    attrs = {
        "resources": attr.string_dict(
            mandatory = True,
            doc = "Dict mapping each resource file to its variable name.",
        ),
        "header_output": attr.string(
            mandatory = True,
            doc = "Path to the header file that will be generated.",
        ),
        "out": attr.output(mandatory = True, doc = "JSON file to generate with resource mapping."),
    },
)

def embed_resources(name, resources, header_output, **kwargs):
    """Embed resources into C++ source files.

    This rule will generate a C++ source file for each input file, and a header file that declares
    the extern data symbols and exposes a `const std::span<const unsigned char>` named
    `donner::embedded::<variable_name>`.
    Args:
        name: The name of the generated target.
        resources: Dict mapping each C++ variable name to the asset file to embed,
            e.g. `{variable_name: src_file}`.
        header_output: Path (relative to the package) of the header to generate.
            The header may be included with `#include "<header_output>"`, and declares a
            `const std::span<const unsigned char>` named `donner::embedded::<variable_name>`.
        **kwargs: Additional keyword arguments passed to cc_library.
    """
    assets = []

    for variable_name, src in sorted(resources.items()):
        filename = _sanitize_filename(src)
        asset_to_cpp(
            name = name + "_embedded_" + filename,
            src = src,
            out = _sanitize_filename(src) + ".cpp",
            variable_name = "__donner_embedded_" + variable_name,
        )
        assets.append(":" + name + "_embedded_" + filename)

    _save_repro_dict(
        name = name + "_repro_json",
        header_output = header_output,
        resources = resources,
        out = name + "_repro.json",
        tags = ["embed_resources_repro"],
    )

    variable_names = [vn for vn, _ in sorted(resources.items())]
    header_output_name = header_output.split("/")[-1]
    header_output_dir = "/".join(header_output.split("/")[:-1])

    embed_resources_generate_header(
        name = name + "_header_gen",
        variable_names = variable_names,
        out = header_output_name,
    )

    cc_library(
        name = name,
        srcs = assets,
        hdrs = [header_output_name],
        include_prefix = header_output_dir,
        **kwargs
    )
