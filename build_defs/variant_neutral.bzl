"""Compile a variant-independent C++ library once per platform.

`variant_neutral_srcs` compiles a library's own sources in the variant-neutral configuration of
`variant_settings.bzl` and returns `srcs` for the library itself, which then links those objects in
the consumer's configuration. Only the library's objects cross the configuration edge: its deps
and its link stay in the consumer's configuration, so no library is linked twice.

Sharing is sound only when a compile of the library is the same in the consumer's configuration
and in the neutral one. The objects rule checks that during analysis, in every configuration that
uses it, and fails the build otherwise:

- The library's compile actions. The neutral compile target is also analyzed, never built, in the
  consumer's configuration. Each compile's arguments and environment must match the neutral
  configuration's apart from the configuration output directory, and so must the inputs the
  target declares: its own sources and headers, the headers of its compilation context, and the
  C++ toolchain's identity and files. Declared inputs are compared rather than `Action.inputs`,
  which Bazel narrows to the discovered headers once a compile has run. This covers command-line
  compile options, features and make variables.
- What those actions read but do not spell out: the library's own compile attributes, the
  compilation context of its deps (defines, include paths, headers), and the action that writes
  each generated header in that context where the deps, srcs and hdrs edges reach it. Every
  generated header must also come from a target on GENERATED_HEADER_ALLOWLIST.

A library that sees a variant define, or a header generated differently per variant, therefore
cannot be shared.

With sharing on, the objects of a wrapped library live under its `_<name>_neutral_compile` target,
not under the library, so they do not share cache entries with builds that leave sharing off.
"""

load("@rules_cc//cc:defs.bzl", "cc_library")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("//build_defs:variant_settings.bzl", "variant_neutral_transition")

# Targets that write headers a wrapped library may include, each with the reason the header does
# not depend on a product variant. A key ending in `*` matches every target name with that prefix.
# Where the aspect below reaches the writing target, its action is also compared per configuration.
GENERATED_HEADER_ALLOWLIST = {
    str(Label("//build_defs:_variant_neutral_generated_header")): "Guard test fixture.",
    str(Label("@zlib//:copy_public_header_")) + "*": "Byte copies of zlib's own source headers.",
}

_COMPILED_EXTENSIONS = ["c", "cc", "cpp", "cxx", "S"]
_HEADER_EXTENSIONS = ["h", "hh", "hpp", "hxx", "inc", "inl", "ipp", "def"]
_COMPILE_MNEMONICS = ["CppCompile", "ObjcCompile", "ObjcppCompile"]

# Attributes whose values reach a compile of the library's own sources.
_COMPILE_ATTRS = ["copts", "defines", "local_defines", "includes", "include_prefix", "strip_include_prefix"]
_COMPILE_DEP_ATTRS = ["deps", "implementation_deps"]
_COMPILE_FILE_ATTRS = ["srcs", "hdrs", "textual_hdrs", "additional_compiler_inputs"]
_TRAVERSED_ATTRS = _COMPILE_DEP_ATTRS + _COMPILE_FILE_ATTRS + ["actual"]

def _normalize(value):
    """Replaces each configuration directory under bazel-out with a placeholder."""
    parts = value.split("bazel-out/")
    result = [parts[0]]
    for part in parts[1:]:
        slash = part.find("/")
        result.append("<config>" + (part[slash:] if slash >= 0 else ""))
    return "bazel-out/".join(result)

def _paths(files):
    return [_normalize(file.path) for file in files]

def _sorted_paths(files):
    return sorted(_paths(files))

_HeaderActionsInfo = provider(
    doc = "Actions in a compile closure that write headers.",
    fields = {"fingerprints": "depset of normalized descriptions of each header-writing action"},
)

def _header_actions_impl(target, ctx):
    fingerprints = []
    for action in target.actions:
        outputs = [file for file in action.outputs.to_list() if file.extension in _HEADER_EXTENSIONS]
        if not outputs:
            continue
        parts = [str(target.label), action.mnemonic, "outputs:"] + _paths(outputs)
        parts += ["inputs:"] + _sorted_paths(action.inputs.to_list())
        parts += ["argv:"] + [_normalize(arg) for arg in (action.argv or [])]
        if action.content != None:
            parts.append("content:" + _normalize(action.content))
        if action.substitutions:
            parts += [
                "substitution:" + key + "=" + _normalize(value)
                for key, value in sorted(action.substitutions.items())
            ]
        fingerprints.append(" ".join(parts))
    transitive = []
    for name in _TRAVERSED_ATTRS:
        values = getattr(ctx.rule.attr, name, None)
        if values == None:
            continue
        for dep in values if type(values) == "list" else [values]:
            if _HeaderActionsInfo in dep:
                transitive.append(dep[_HeaderActionsInfo].fingerprints)
    return [_HeaderActionsInfo(fingerprints = depset(fingerprints, transitive = transitive))]

_header_actions = aspect(
    implementation = _header_actions_impl,
    attr_aspects = _TRAVERSED_ATTRS,
)

def _allowlisted(owner):
    for key in GENERATED_HEADER_ALLOWLIST:
        if owner == key or (key.endswith("*") and owner.startswith(key[:-1])):
            return True
    return False

def _check_generated_headers(library, files):
    """Fails unless each generated header comes from an allowlisted target.

    Headers under _virtual_includes are links that cc_library makes to its own headers; a
    generated header behind such a link is itself in the compilation context and checked there.
    """
    for file in files:
        if file.is_source or "/_virtual_includes/" in file.path:
            continue
        owner = str(file.owner)
        if not _allowlisted(owner):
            fail(("{} sees the generated header {} from {}, which is not on " +
                  "GENERATED_HEADER_ALLOWLIST in build_defs/variant_neutral.bzl. Add it with the " +
                  "reason its content cannot depend on a product variant, or remove " +
                  "variant_neutral.").format(library, _normalize(file.path), owner))

_CompileInputsInfo = provider(
    doc = "What a compile of a library's own sources reads, with configuration paths normalized.",
    fields = {"fingerprint": "list of strings"},
)

def _context_lines(name, context):
    lines = ["%s define: %s" % (name, value) for value in context.defines.to_list()]
    for field in ["includes", "quote_includes", "system_includes", "framework_includes", "external_includes"]:
        values = getattr(context, field, None)
        if values != None:
            lines += ["%s %s: %s" % (name, field, _normalize(value)) for value in values.to_list()]
    lines += ["%s header: %s" % (name, path) for path in _sorted_paths(context.headers.to_list())]
    return lines

def _compile_inputs_impl(ctx):
    lines = ["feature: " + feature for feature in sorted(ctx.features)]
    lines += ["disabled feature: " + feature for feature in sorted(ctx.disabled_features)]
    for name in _COMPILE_ATTRS:
        value = getattr(ctx.attr, name)
        for item in value if type(value) == "list" else [value]:
            lines.append("%s: %s" % (name, _normalize(item)))
    files = []
    for name in _COMPILE_FILE_ATTRS:
        files += getattr(ctx.files, name)
        lines += ["%s: %s" % (name, path) for path in _paths(getattr(ctx.files, name))]
    for name in _COMPILE_DEP_ATTRS:
        context = cc_common.merge_compilation_contexts(
            compilation_contexts = [dep[CcInfo].compilation_context for dep in getattr(ctx.attr, name)],
        )
        files += context.headers.to_list()
        lines += _context_lines(name, context)
    _check_generated_headers(ctx.attr.library_label, files)
    fingerprints = []
    for name in _COMPILE_DEP_ATTRS + _COMPILE_FILE_ATTRS:
        for dep in getattr(ctx.attr, name):
            if _HeaderActionsInfo in dep:
                fingerprints.append(dep[_HeaderActionsInfo].fingerprints)
    lines += ["header action: " + line for line in sorted(depset(transitive = fingerprints).to_list())]
    return [_CompileInputsInfo(fingerprint = lines)]

_compile_inputs = rule(
    implementation = _compile_inputs_impl,
    attrs = {
        "additional_compiler_inputs": attr.label_list(allow_files = True, aspects = [_header_actions]),
        "copts": attr.string_list(),
        "defines": attr.string_list(),
        "deps": attr.label_list(providers = [CcInfo], aspects = [_header_actions]),
        "hdrs": attr.label_list(allow_files = True, aspects = [_header_actions]),
        "implementation_deps": attr.label_list(providers = [CcInfo], aspects = [_header_actions]),
        "include_prefix": attr.string(),
        "includes": attr.string_list(),
        "library_label": attr.string(mandatory = True),
        "local_defines": attr.string_list(),
        "srcs": attr.label_list(allow_files = True, aspects = [_header_actions]),
        "strip_include_prefix": attr.string(),
        "textual_hdrs": attr.label_list(allow_files = True, aspects = [_header_actions]),
    },
    doc = "Collects what a compile of the library's own sources reads in this configuration.",
)

_CompileActionsInfo = provider(
    doc = "A C++ target's compile commands and the inputs it declares for them.",
    fields = {
        "commands": "dict of normalized output path to a list of argv and env lines",
        "declared": "sorted lines naming the declared inputs and the C++ toolchain",
    },
)

_CPP_TOOLCHAIN_TYPE = "@bazel_tools//tools/cpp:toolchain_type"

def _toolchain_lines(ctx):
    toolchain = None
    toolchains = getattr(ctx.rule, "toolchains", None)
    if toolchains != None and _CPP_TOOLCHAIN_TYPE in toolchains:
        toolchain = getattr(toolchains[_CPP_TOOLCHAIN_TYPE], "cc", None)
    legacy = getattr(ctx.rule.attr, "_cc_toolchain", None)
    if toolchain == None and legacy != None and cc_common.CcToolchainInfo in legacy:
        toolchain = legacy[cc_common.CcToolchainInfo]
    if toolchain == None:
        fail("variant_neutral cannot find the C++ toolchain of " + str(ctx.label))
    lines = ["toolchain: " + toolchain.toolchain_id, "compiler: " + _normalize(toolchain.compiler_executable)]
    return lines + ["toolchain file: " + path for path in _sorted_paths(toolchain.all_files.to_list())]

def _compile_actions_impl(target, ctx):
    commands = {}
    for action in target.actions:
        if action.mnemonic in _COMPILE_MNEMONICS:
            lines = ["argv: " + _normalize(arg) for arg in action.argv]
            lines += ["env: %s=%s" % (key, _normalize(value)) for key, value in sorted(action.env.items())]
            commands[_normalize(action.outputs.to_list()[0].path)] = lines
    files = []
    for name in _COMPILE_FILE_ATTRS:
        files += getattr(ctx.rule.files, name, [])
    declared = ["input: " + path for path in _sorted_paths(files)]
    declared += ["header: " + path for path in _sorted_paths(target[CcInfo].compilation_context.headers.to_list())]
    return [_CompileActionsInfo(commands = commands, declared = declared + _toolchain_lines(ctx))]

_compile_actions = aspect(implementation = _compile_actions_impl)

def _difference(left, right):
    remaining = {line: True for line in right}
    return [line for line in left if line not in remaining]

def _mismatch(library, consumer, neutral, what = ""):
    only_consumer = _difference(consumer, neutral)
    only_neutral = _difference(neutral, consumer)
    fail(("{} compiles differently in this configuration than in the variant-neutral " +
          "configuration, so it cannot share one compile across variants. Remove " +
          "variant_neutral, or keep variant settings out of its compile.{}\n" +
          "Only in this configuration:\n  {}\nOnly in the neutral configuration:\n  {}{}").format(
        library,
        what,
        "\n  ".join(only_consumer[:20]),
        "\n  ".join(only_neutral[:20]),
        "" if only_consumer or only_neutral else "\n(same entries in a different order)",
    ))

def _compare_compile_actions(library, consumer, neutral):
    if sorted(consumer.commands.keys()) != sorted(neutral.commands.keys()):
        _mismatch(
            library,
            ["compile: " + key for key in sorted(consumer.commands.keys())],
            ["compile: " + key for key in sorted(neutral.commands.keys())],
        )
    for output in sorted(neutral.commands.keys()):
        if consumer.commands[output] != neutral.commands[output]:
            _mismatch(library, consumer.commands[output], neutral.commands[output], " Compile of " + output + ":")
    if consumer.declared != neutral.declared:
        _mismatch(library, consumer.declared, neutral.declared, " Declared compile inputs:")

def _object_stem(basename):
    return basename.removesuffix(".o").removesuffix(".pic")

def _variant_neutral_objects_impl(ctx):
    library = ctx.attr.library_label
    neutral = ctx.attr.neutral_inputs[0][_CompileInputsInfo].fingerprint
    consumer = ctx.attr.consumer_inputs[_CompileInputsInfo].fingerprint
    if neutral != consumer:
        _mismatch(library, consumer, neutral)
    neutral_library = ctx.attr.library[0]
    _compare_compile_actions(
        library,
        ctx.attr.consumer_library[_CompileActionsInfo],
        neutral_library[_CompileActionsInfo],
    )
    objects = [
        file
        for file in neutral_library[OutputGroupInfo].compilation_outputs.to_list()
        if file.extension == "o"
    ]
    stems = sorted({_object_stem(file.basename): True for file in objects}.keys())
    if stems != sorted({stem: True for stem in ctx.attr.source_stems}.keys()):
        fail("expected objects for {}, found {}".format(ctx.attr.source_stems, [file.basename for file in objects]))
    return [DefaultInfo(files = depset(objects))]

_variant_neutral_objects = rule(
    implementation = _variant_neutral_objects_impl,
    attrs = {
        # Analyzed for comparison only; nothing in this configuration builds it.
        "consumer_library": attr.label(mandatory = True, aspects = [_compile_actions]),
        "consumer_inputs": attr.label(mandatory = True, providers = [_CompileInputsInfo]),
        "library": attr.label(
            mandatory = True,
            cfg = variant_neutral_transition,
            aspects = [_compile_actions],
            providers = [OutputGroupInfo],
        ),
        "library_label": attr.string(mandatory = True),
        "neutral_inputs": attr.label(mandatory = True, cfg = variant_neutral_transition, providers = [_CompileInputsInfo]),
        "source_stems": attr.string_list(mandatory = True),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
    },
    doc = "The library's objects from the variant-neutral configuration, after the compile check.",
)

def _extension(path):
    basename = path.rpartition("/")[2]
    return basename.rpartition(".")[2] if "." in basename else ""

def variant_neutral_srcs(name, srcs, hdrs = [], copts = [], testonly = None, target_compatible_with = None, **kwargs):
    """Declares the neutral compile of a library and returns the `srcs` for the library itself.

    The returned value compiles `srcs` in place unless `//build_defs:share_variant_neutral_compiles`
    is set on a native platform without coverage collection. Then the library links its objects
    from the variant-neutral configuration, which fails analysis if the compile would differ.

    Args:
      name: The library's name.
      srcs: The library's sources: files in this package, no select().
      hdrs: The library's headers.
      copts: The library's compile options.
      testonly: The library's testonly value.
      target_compatible_with: The library's compatibility constraints.
      **kwargs: The library's other cc_library attributes. Those that affect compilation are
        applied to the neutral compile and compared.

    Returns:
      The `srcs` value for the library's cc_library.
    """
    if type(srcs) != "list":
        fail("variant_neutral needs a plain srcs list: " + name)
    compiled = []
    headers = []
    for src in srcs:
        if ":" in src:
            fail("variant_neutral needs source files in this package, not " + src)
        extension = _extension(src)
        if extension in _COMPILED_EXTENSIONS:
            compiled.append(src)
        elif extension in _HEADER_EXTENSIONS:
            headers.append(src)
        else:
            fail("variant_neutral cannot classify " + src)
    if not compiled:
        fail("variant_neutral has nothing to compile for " + name)

    compile_kwargs = {
        key: kwargs[key]
        for key in _COMPILE_ATTRS + _COMPILE_DEP_ATTRS + ["features", "textual_hdrs", "additional_compiler_inputs", "toolchains"]
        if key in kwargs
    }
    common = {
        "tags": ["manual"],
        "target_compatible_with": target_compatible_with,
        "testonly": testonly,
        "visibility": ["//visibility:private"],
    }
    library_label = str(native.package_relative_label(name))
    cc_library(
        name = "_" + name + "_neutral_compile",
        srcs = srcs,
        hdrs = hdrs,
        copts = copts,
        **(compile_kwargs | common)
    )
    _compile_inputs(
        name = "_" + name + "_compile_inputs",
        srcs = srcs,
        hdrs = hdrs,
        copts = copts,
        library_label = library_label,
        **(compile_kwargs | common)
    )
    _variant_neutral_objects(
        name = "_" + name + "_neutral_objects",
        consumer_inputs = ":_" + name + "_compile_inputs",
        consumer_library = ":_" + name + "_neutral_compile",
        library = ":_" + name + "_neutral_compile",
        library_label = library_label,
        neutral_inputs = ":_" + name + "_compile_inputs",
        source_stems = [src.rpartition("/")[2].rpartition(".")[0] for src in compiled],
        **common
    )
    shared = headers + [":_" + name + "_neutral_objects"]
    return select({
        Label("//build_defs:share_variant_neutral_compiles_linux"): shared,
        Label("//build_defs:share_variant_neutral_compiles_macos"): shared,
        "//conditions:default": srcs,
    })
