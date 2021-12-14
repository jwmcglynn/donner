load("@rules_cc//cc:defs.bzl", "cc_test")

def donner_cc_fuzzer(name, corpus, **kwargs):
    """
    Create a libfuzzer-based fuzz target.

    Args:
      name: Rule name.
      corpus: Path to a corpus directory, or a filegroup rule for the corpus.
      **kwargs: Additional arguments, matching the implementation of cc_test.
    """
    if not (corpus.startswith("//") or corpus.startswith(":")):
        corpus_name = name + "_corpus"
        corpus = native.glob([corpus + "/**"])
        native.filegroup(name = corpus_name, srcs = corpus)
    else:
        corpus_name = corpus

    cc_test(
        name = name,
        linkopts = ["-fsanitize=fuzzer"],
        args = ["$(locations %s)" % corpus_name],
        linkstatic = 1,
        data = [corpus_name],
        # Only run on Linux, since the macOS clang is missing libclang_rt.fuzzer_osx.a.
        target_compatible_with = [
            "@platforms//os:linux",
        ],
        **kwargs
    )

def _copy_filegroup_impl(ctx):
    all_input_files = ctx.attr.filegroup.files.to_list()

    all_outputs = []
    for input_file in all_input_files:
        output = input_file.short_path
        if not output.startswith(ctx.attr.strip_prefix):
            fail("Input file does not start with prefix, file={}, prefix={}".format(
                output,
                ctx.attr.strip_prefix))
        output = output[len(ctx.attr.strip_prefix):]

        output_file = ctx.actions.declare_file(output)
        all_outputs += [output_file]
        ctx.actions.run_shell(
            outputs=[output_file],
            inputs=depset([input_file]),
            arguments=[input_file.path, output_file.path],
            command="cp $1 $2")

    return [
        DefaultInfo(
            files=depset(all_outputs),
            runfiles=ctx.runfiles(files=all_outputs))
    ]

copy_filegroup = rule(
    implementation=_copy_filegroup_impl,
    attrs={
        "filegroup": attr.label(),
        "strip_prefix": attr.string(),
    },
)

def copy_pathfinder_resources(name):
    """
    Copy pathfinder resources to output directory.

    Args:
      name: Rule name.
    """
    copy_filegroup(
        name = name,
        filegroup = "//third_party/pathfinder_wrapper:pathfinder_resources_filegroup",
        strip_prefix = "../raze__pathfinder_resources__0_5_0/"
    )
