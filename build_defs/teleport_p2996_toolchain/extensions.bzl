"""Developer-local Bloomberg clang-p2996 repository wiring for Teleport."""

_LLVM_REPO_BUILD_TEMPLATE = """package(default_visibility = ["//visibility:public"])

exports_files(glob(
    [
        "bin/*",
        "lib/**",
        "include/**",
        "share/clang/*",
    ],
    allow_empty = True,
))

filegroup(name = "root", srcs = [])

filegroup(
    name = "clang",
    srcs = [
        "bin/clang",
        "bin/clang++",
        "bin/clang-cpp",
    ],
)

filegroup(
    name = "ld",
    srcs = [
        "bin/ld.lld",
        "bin/ld64.lld",
    ] + glob(["bin/wasm-ld"], allow_empty = True),
)

filegroup(
    name = "include",
    srcs = glob(
        [
            "include/**/c++/**",
            "lib/clang/{llvm_major_version}/include/**",
        ],
        allow_empty = True,
    ),
)

filegroup(
    name = "all_includes",
    srcs = glob(["include/**"], allow_empty = True),
)

filegroup(
    name = "cxx_builtin_include",
    srcs = [
        "include/c++",
        "lib/clang/{llvm_major_version}/include",
    ],
)

filegroup(
    name = "extra_config_site",
    srcs = glob(["include/*/c++/v1/__config_site"], allow_empty = True),
)

filegroup(name = "bin", srcs = glob(["bin/**"], allow_empty = True))

filegroup(
    name = "lib",
    srcs = [
        "lib/clang/{llvm_major_version}/lib",
    ] + glob(
        [
            "lib/**/libc++*.a",
            "lib/**/libunwind.a",
        ],
        allow_empty = True,
    ),
)

filegroup(
    name = "lib_legacy",
    srcs = glob(
        [
            "lib/clang/{llvm_major_version}/lib/**",
            "lib/**/libc++*.a",
            "lib/**/libunwind.a",
        ],
        allow_empty = True,
    ),
)

filegroup(name = "ar", srcs = ["bin/llvm-ar"])
filegroup(name = "as", srcs = ["bin/clang", "bin/llvm-as"])
filegroup(name = "nm", srcs = ["bin/llvm-nm"])
filegroup(name = "objcopy", srcs = ["bin/llvm-objcopy"])
filegroup(name = "objdump", srcs = ["bin/llvm-objdump"])
filegroup(name = "profdata", srcs = ["bin/llvm-profdata"])
filegroup(name = "dwp", srcs = ["bin/llvm-dwp"])
filegroup(name = "ranlib", srcs = ["bin/llvm-ranlib"])
filegroup(name = "readelf", srcs = ["bin/llvm-readelf"])
filegroup(name = "strip", srcs = ["bin/llvm-strip"])
filegroup(name = "symbolizer", srcs = ["bin/llvm-symbolizer"])
filegroup(name = "clang-tidy", srcs = ["bin/clang-tidy"])
filegroup(name = "clang-format", srcs = ["bin/clang-format"])
filegroup(name = "git-clang-format", srcs = ["bin/git-clang-format"])

filegroup(
    name = "libclang",
    srcs = glob(
        [
            "lib/libclang.so",
            "lib/libclang.dylib",
        ],
        allow_empty = True,
    ),
)
"""

_PLACEHOLDER_TOOLS = [
    "clang",
    "clang++",
    "clang-cpp",
    "clang-format",
    "clang-tidy",
    "git-clang-format",
    "ld.lld",
    "ld64.lld",
    "llvm-ar",
    "llvm-as",
    "llvm-cov",
    "llvm-dwp",
    "llvm-nm",
    "llvm-objcopy",
    "llvm-objdump",
    "llvm-profdata",
    "llvm-ranlib",
    "llvm-readelf",
    "llvm-strip",
    "llvm-symbolizer",
]

def _missing_tool_script(tool_name, message):
    return """#!/bin/sh
echo "{message}" >&2
echo "Requested tool: {tool_name}" >&2
exit 1
""".format(
        message = message.replace("\"", "\\\""),
        tool_name = tool_name,
    )

def _write_placeholder_root(repository_ctx, message):
    repository_ctx.file("BUILD.bazel", _LLVM_REPO_BUILD_TEMPLATE.format(
        llvm_major_version = repository_ctx.attr.llvm_major_version,
    ))
    repository_ctx.file("README.teleport_p2996.txt", message + "\n")

    for directory in [
        "bin",
        "include/c++",
        "lib/clang/{}/include".format(repository_ctx.attr.llvm_major_version),
        "lib/clang/{}/lib".format(repository_ctx.attr.llvm_major_version),
        "share/clang/{}".format(repository_ctx.attr.llvm_major_version),
    ]:
        repository_ctx.file(directory + "/.keep", "")

    for tool_name in _PLACEHOLDER_TOOLS:
        repository_ctx.file(
            "bin/" + tool_name,
            _missing_tool_script(tool_name, message),
            executable = True,
        )

def _teleport_p2996_root_impl(repository_ctx):
    env_value = repository_ctx.os.environ.get(repository_ctx.attr.env_var, "")
    root_path = env_value or repository_ctx.attr.default_path

    if not root_path:
        _write_placeholder_root(
            repository_ctx,
            (
                "Teleport P2996 toolchain is not configured. Set {env_var} or install the " +
                "fork under {default_path}, then build with --config=teleport_spike."
            ).format(
                env_var = repository_ctx.attr.env_var,
                default_path = repository_ctx.attr.default_path,
            ),
        )
        return

    root = repository_ctx.path(root_path)
    if not root.exists:
        if env_value:
            fail(
                "Environment variable {} points to {!r}, but that path does not exist.".format(
                    repository_ctx.attr.env_var,
                    root_path,
                ),
            )

        _write_placeholder_root(
            repository_ctx,
            (
                "Teleport P2996 toolchain was not found at the default path {!r}. Set {} " +
                "or install the fork there, then build with --config=teleport_spike."
            ).format(
                root_path,
                repository_ctx.attr.env_var,
            ),
        )
        return

    required_paths = [
        "bin/clang",
        "bin/clang++",
        "bin/clang-cpp",
        "bin/ld.lld",
        "include/c++",
        "lib/clang/{}/include".format(repository_ctx.attr.llvm_major_version),
        "lib/clang/{}/lib".format(repository_ctx.attr.llvm_major_version),
        "lib",
    ]
    missing = [
        rel for rel in required_paths
        if not repository_ctx.path(root_path + "/" + rel).exists
    ]
    if missing:
        fail(
            "Teleport P2996 root '{}' is missing required paths: {}".format(
                root_path,
                ", ".join(missing),
            ),
        )

    repository_ctx.file(
        "BUILD.bazel",
        _LLVM_REPO_BUILD_TEMPLATE.format(
            llvm_major_version = repository_ctx.attr.llvm_major_version,
        ),
    )
    repository_ctx.file(
        "README.teleport_p2996.txt",
        "Using local Teleport P2996 toolchain root: {}\n".format(root_path),
    )

    for entry in ["bin", "include", "lib", "share"]:
        entry_path = repository_ctx.path(root_path + "/" + entry)
        if entry_path.exists:
            repository_ctx.symlink(entry_path, entry)

teleport_p2996_root_repository = repository_rule(
    implementation = _teleport_p2996_root_impl,
    attrs = {
        "default_path": attr.string(default = "/opt/p2996/clang"),
        "env_var": attr.string(default = "TELEPORT_P2996_ROOT"),
        "llvm_major_version": attr.string(default = "21"),
    },
    environ = ["TELEPORT_P2996_ROOT"],
)

def _teleport_p2996_impl(module_ctx):
    for mod in module_ctx.modules:
        if not mod.is_root:
            fail("Only the root module can use the 'teleport_p2996' extension")

        for local_root in mod.tags.local_root:
            teleport_p2996_root_repository(
                name = local_root.name,
                default_path = local_root.default_path,
                env_var = local_root.env_var,
                llvm_major_version = local_root.llvm_major_version,
            )

teleport_p2996 = module_extension(
    implementation = _teleport_p2996_impl,
    tag_classes = {
        "local_root": tag_class(
            attrs = {
                "default_path": attr.string(default = "/opt/p2996/clang"),
                "env_var": attr.string(default = "TELEPORT_P2996_ROOT"),
                "llvm_major_version": attr.string(default = "21"),
                "name": attr.string(default = "teleport_p2996_llvm_root"),
            },
        ),
    },
)
