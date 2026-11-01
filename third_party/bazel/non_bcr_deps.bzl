"""Module extension for non-BCR dependencies.

These repos are fetched only when donner is the root module (i.e. local
development or CI), not when donner is consumed as a dependency from the
Bazel Central Registry. The extension is imported from //MODULE.bazel with
`dev_dependency = True`, so downstream BCR consumers never see these repos.

Each dep listed here is load-bearing for a feature that is opt-in and NOT
shipped over BCR:

- sheenbidi_unicode_data: Unicode BiDi conformance data (tests only)
- harfbuzz        : Text shaping                (--config=text-full)
- woff2           : WOFF2 font format           (--config=text-full)
- wgpu_native     : WebGPU (Geode renderer)     (--//donner/svg/renderer/geode:enable_geode=true)
- tracy           : In-process profiling client (//donner/editor only — see check_banned_patterns.py)
- bazel_clang_tidy: clang-tidy aspect           (--config=clang-tidy)
- clang_format_*: Pinned C++ formatter       (//tools/format, checks on in donner's .bazelrc)
- sysroot_linux_*: Hermetic Debian Bullseye sysroots for Bazel remote execution
                   (--config=re). Wired into the `llvm_toolchain` via its
                   `sysroot`, `extra_compiler_files` and `extra_linker_files`
                   tags so RE workers without glibc headers can still compile.

When / how to add a new non-BCR dep:
  1. If the dep is load-bearing for the default tiny-skia + text-base build,
     it MUST NOT live here — it must either be on BCR (as a bazel_dep), or
     vendored under third_party/ via `new_local_repository` so it ships in
     the source archive.
  2. If the dep is gated behind a config_setting / bool_flag AND every BUILD
     target referencing it has `target_compatible_with = <flag>_enabled`,
     add it to `_non_bcr_deps_impl` below.
  3. Update the checklist in docs/design_docs/0018-bcr_release.md (when that
     document exists — tracked under the BCR release plan).

See docs/design_docs/0018-bcr_release.md for the fuller picture of what ships on
BCR versus what stays behind `git_override`.
"""

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository", "new_git_repository")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def _non_bcr_deps_impl(_mctx):
    # Data only; runtime SheenBidi is the regular BCR module in MODULE.bazel.
    http_archive(
        name = "sheenbidi_unicode_data",
        build_file = "//third_party:BUILD.bidi_unicode_data",
        sha256 = "86c56014034739ba39a24c23eb00323b0bf6f737354f665786015fca842af786",
        strip_prefix = "SheenBidi-3.0.0",
        urls = ["https://github.com/Tehreer/SheenBidi/archive/refs/tags/v3.0.0.tar.gz"],
    )

    # WOFF2 text support. Gated on //donner/svg/renderer:text_full_enabled.
    new_git_repository(
        name = "woff2",
        build_file = "//third_party:BUILD.woff2",
        commit = "fb9c3379f2605b10f3e8f1d9636664ab5576775c",
        patches = ["//third_party:woff2_bounded_decode.patch"],
        patch_args = ["-p1"],
        remote = "https://github.com/google/woff2.git",
    )

    # HarfBuzz text shaping. Gated on //donner/svg/renderer:text_full_enabled.
    #
    # Fetched as the release tag's source archive and pinned by its SHA-256, so a
    # moved tag fails the fetch instead of changing the shipped sources. The
    # archive holds the same tree as a checkout of the tag.
    #
    # The patch_cmds below create src/config-override.h to re-enable parts of
    # the HarfBuzz API that HB_TINY/HB_LEAN strip out. We need:
    #   - file I/O for hb_face_create_from_file_or_fail
    #   - vertical advances and per-glyph origins from FreeType
    http_archive(
        name = "harfbuzz",
        build_file = "//third_party:BUILD.harfbuzz",
        sha256 = "3c2a9006a7e1bf58737e557014d7882c554c628fb379a9f00008f5ea53dbbdfb",
        strip_prefix = "harfbuzz-14.2.1",
        patches = ["//third_party:harfbuzz_vertical_no_var_guard.patch"],
        patch_args = ["-p1"],
        urls = ["https://github.com/harfbuzz/harfbuzz/archive/refs/tags/14.2.1.tar.gz"],
        patch_cmds = [
            """cat > src/config-override.h << 'HBEOF'
// Re-enable file I/O (hb_face_create_from_file_or_fail references
// hb_blob_create_from_file_or_fail which is guarded by HB_NO_OPEN).
#undef HB_NO_OPEN
// Preserve font-provided vertical advances and per-glyph origins.
#undef HB_NO_VERTICAL
// Keep variation composite decoding outside the shaping capability set.
#define HB_NO_VAR_COMPOSITES
HBEOF""",
        ],
    )

    # Pinned wgpu-native archives for the Linux-only resvg pixel comparison. Ordinary native
    # Geode, editor and renderer targets have no edge to these repositories.
    # Only the Linux resvg comparison retains a wgpu-native archive. The alias in
    # third_party/webgpu-cpp/BUILD.bazel selects its two supported CPU architectures.
    # Tag `v24.0.3.1` is pinned because eliemichel/WebGPU-distribution's
    # vendored `webgpu.hpp` tracks wgpu-native's v24 C API (see
    # `wgpu-native-git-tag.txt` in their repo). Bumping wgpu-native past
    # v24 needs a matching `webgpu.hpp` regeneration; bump deliberately.
    # To refresh shasums, run `shasum -a 256` against the release zip and
    # paste the result into the `sha256` below.
    _WGPU_NATIVE_VERSION = "v24.0.3.1"
    _WGPU_NATIVE_PLATFORMS = [
        struct(
            name = "wgpu_native_linux_x86_64",
            asset = "wgpu-linux-x86_64-release.zip",
            sha256 = "86f3eb9f74d7f1ac82ee52d9b2ab15e366ef86a932759c750b7472652836ee59",
        ),
        struct(
            name = "wgpu_native_linux_aarch64",
            asset = "wgpu-linux-aarch64-release.zip",
            sha256 = "97786f622d6d4f9aaa87c27d165de8db65daf1d391e0bcc32a2dd9bb45fcd299",
        ),
    ]
    for p in _WGPU_NATIVE_PLATFORMS:
        http_archive(
            name = p.name,
            url = "https://github.com/gfx-rs/wgpu-native/releases/download/{}/{}".format(
                _WGPU_NATIVE_VERSION,
                p.asset,
            ),
            sha256 = p.sha256,
            build_file = "//third_party:BUILD.wgpu_native_platform",
        )

    # Tracy in-process profiler. Only consumed under //donner/editor/...
    # Donner uses a custom BUILD file (third_party/BUILD.tracy) because Tracy
    # does not ship Bazel BUILD files upstream. The build file is adapted
    # from the one donner-editor maintains in its own vendored Tracy copy.
    # Pinned to the latest stable upstream release's tag archive and its
    # SHA-256, like HarfBuzz above. Bump deliberately.
    http_archive(
        name = "tracy",
        build_file = "//third_party:BUILD.tracy",
        sha256 = "d4efc50ebcb0bfcfdbba148995aeb75044c0d80f5d91223aebfaa8fa9e563d2b",
        strip_prefix = "tracy-0.13.1",
        urls = ["https://github.com/wolfpld/tracy/archive/refs/tags/v0.13.1.tar.gz"],
    )

    # bazel_clang_tidy: --config=clang-tidy aspect (see .bazelrc).
    git_repository(
        name = "bazel_clang_tidy",
        commit = "c4d35e0d0b838309358e57a2efed831780f85cd0",
        remote = "https://github.com/erenon/bazel_clang_tidy.git",
    )

    # Hermetic Linux sysroots for Bazel remote execution via `--config=re`.
    #
    # A remote-execution worker may have no system glibc headers, so
    # libc++'s <wchar.h> can't find `mbstate_t`. Chromium's prebuilt
    # Debian Bullseye sysroots give the LLVM toolchain a complete set of
    # C/C++ headers + runtime stubs to link against, keeping the build
    # hermetic regardless of what the RE host does (or does not) install.
    #
    # Sourced from https://commondatastorage.googleapis.com/chrome-linux-sysroot
    # (URL + "/" + Sha256Sum; see Chromium's build/linux/sysroot_scripts/
    # install-sysroot.py). Bump by pulling the latest sysroots.json from
    # chromium/src and updating both the sha256 + URL fragment.
    #
    # The sysroot holds about 18,000 files, most of them for packages the build
    # never touches (Qt, GTK, Python, Debian package metadata). A remote
    # executor stages every declared input file for every action, so MODULE.bazel
    # gives each action kind only the files it reads:
    # - `sysroot` only names the --sysroot directory and declares no files.
    # - `compiler_files` holds the headers compiles include, plus the GCC
    #   installation files clang inspects. An #include of a header outside it
    #   fails with "file not found"; add its directory here rather than
    #   widening the glob to `**`. A __has_include() test or a compiler driver
    #   check for a file outside it fails silently instead and can change the
    #   output, so compare objects built before and after narrowing it.
    # - `linker_files` holds the C runtime objects and the libraries in the
    #   directories the linker searches.
    _SYSROOT_BUILD_FILE = """\
filegroup(
    name = "sysroot",
    srcs = [],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "compiler_files",
    srcs = glob([
        # The headers directly in usr/include, mostly the C library's, and the
        # C library and Linux kernel interface header directories.
        "usr/include/*",
        "usr/include/*-linux-gnu/*",
        "usr/include/*-linux-gnu/asm/**",
        "usr/include/*-linux-gnu/bits/**",
        "usr/include/*-linux-gnu/gnu/**",
        "usr/include/*-linux-gnu/sys/**",
        "usr/include/arpa/**",
        "usr/include/asm-generic/**",
        "usr/include/linux/**",
        "usr/include/net/**",
        "usr/include/netinet/**",
        "usr/include/netpacket/**",
        "usr/include/rpc/**",
        # X11 and OpenGL, for glfw's X11 backend.
        "usr/include/GL/**",
        "usr/include/KHR/**",
        "usr/include/X11/**",
        # Clang recognizes the GCC installation by its crtbegin.o and derives
        # code generation defaults from its version, such as outline atomics
        # on AArch64. Without these files compiles still succeed but emit
        # different code.
        "usr/lib/gcc/*/*/*",
    ]),
    visibility = ["//visibility:public"],
)

filegroup(
    name = "linker_files",
    srcs = glob([
        # Libraries and C runtime objects in the library search directories.
        "lib/*-linux-gnu/*",
        "usr/lib/*-linux-gnu/*",
        # GCC's runtime objects and support libraries.
        "usr/lib/gcc/*/*/*",
    ]) + glob(
        # The dynamic loader named by the C library's linker script: lib/ on
        # aarch64, lib64/ on x86_64.
        [
            "lib/ld-linux*",
            "lib64/ld-linux*",
        ],
        allow_empty = True,
    ),
    visibility = ["//visibility:public"],
)
"""
    http_archive(
        name = "sysroot_linux_x86_64",
        build_file_content = _SYSROOT_BUILD_FILE,
        sha256 = "52d61d4446ffebfaa3dda2cd02da4ab4876ff237853f46d273e7f9b666652e1d",
        # GCS serves the archive at a bare sha256 path with no extension, so
        # Bazel can't infer the format — pin `type` explicitly.
        type = "tar.xz",
        urls = ["https://commondatastorage.googleapis.com/chrome-linux-sysroot/52d61d4446ffebfaa3dda2cd02da4ab4876ff237853f46d273e7f9b666652e1d"],
    )
    http_archive(
        name = "sysroot_linux_aarch64",
        build_file_content = _SYSROOT_BUILD_FILE,
        sha256 = "c7176a4c7aacbf46bda58a029f39f79a68008d3dee6518f154dcf5161a5486d8",
        type = "tar.xz",
        urls = ["https://commondatastorage.googleapis.com/chrome-linux-sysroot/c7176a4c7aacbf46bda58a029f39f79a68008d3dee6518f154dcf5161a5486d8"],
    )

    # clang-format 21.1.6, the repository's C++ formatter (//tools/format:clang_format). Each
    # repository is one platform's wheel of the PyPI `clang-format` 21.1.6 release, a standalone
    # clang-format built from LLVM tag `llvmorg-21.1.6`, pinned by its version-named file and
    # SHA-256. The wheels are built from the LLVM sources by the third-party clang-format wheel
    # project on PyPI, not published by LLVM; their output matched the LLVM 21.1.6 toolchain's
    # clang-format byte for byte when pinned. The `{name}_format` validations (gated on
    # //tools/format:check, which donner's .bazelrc enables), the fixture test and
    # tools/format/clang_format.sh use it.
    _CLANG_FORMAT_WHEELS = [
        struct(
            name = "clang_format_macos_arm64",
            path = "f8/94/46762b38563b4aea840c706149f7d5b3eeed450c0ba874bb77b0de635674",
            tag = "macosx_11_0_arm64",
            sha256 = "74880cb25886c1bb4a95307efeb40f19eba3f1fd86ddee88166814b9ed2600ef",
        ),
        struct(
            name = "clang_format_linux_aarch64",
            path = "51/59/0fc0ae742a5156ae9af1fb80359557ebb8e52096a2bfcb46e889728c15b7",
            tag = "manylinux_2_26_aarch64.manylinux_2_28_aarch64",
            sha256 = "20102f69df02282280d5c7dac682987cb82fee9ff89c034c653154019b17a5ef",
        ),
        struct(
            name = "clang_format_linux_x86_64",
            path = "01/75/e7d82087df0922054f08351ce5e3528d0b0ce64dcc140c00ec1df016a0c6",
            tag = "manylinux_2_27_x86_64.manylinux_2_28_x86_64",
            sha256 = "8a9d82d1d67ffdc491a0a1ce04d7740b6e7ccb1d9b6d2ab90b009040614030e9",
        ),
    ]
    for wheel in _CLANG_FORMAT_WHEELS:
        http_archive(
            name = wheel.name,
            build_file = "//third_party:BUILD.clang_format_wheel",
            sha256 = wheel.sha256,
            type = "zip",
            urls = ["https://files.pythonhosted.org/packages/{}/clang_format-21.1.6-py2.py3-none-{}.whl".format(
                wheel.path,
                wheel.tag,
            )],
        )

non_bcr_deps = module_extension(
    implementation = _non_bcr_deps_impl,
)
