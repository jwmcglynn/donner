"""Module extension for non-BCR dependencies.

These repos are fetched only when donner is the root module (i.e. local
development or CI), not when donner is consumed as a dependency from the
Bazel Central Registry. The extension is imported from //MODULE.bazel with
`dev_dependency = True`, so downstream BCR consumers never see these repos.

Each dep listed here is load-bearing for a feature that is opt-in and NOT
shipped over BCR:

- harfbuzz        : Text shaping                (--config=text-full)
- woff2           : WOFF2 font format           (--config=text-full)
- wgpu_native     : WebGPU (Geode renderer)     (--//donner/svg/renderer/geode:enable_geode=true)
- tracy           : In-process profiling client (//donner/editor only — see check_banned_patterns.py)
- resvg-test-suite: Reference SVG goldens       (image comparison tests)
- bazel_clang_tidy: clang-tidy aspect           (--config=clang-tidy)
- sysroot_linux_*: Hermetic Debian Bullseye sysroots for Bazel remote execution
                   (--config=re). Wired into the `llvm_toolchain` via its
                   `sysroot` parameter so RE workers without glibc headers
                   can still compile.

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
    # WOFF2 text support. Gated on //donner/svg/renderer:text_full_enabled.
    new_git_repository(
        name = "woff2",
        build_file = "//third_party:BUILD.woff2",
        commit = "1f184d05566b3e25827a1f8e68eb82b9ccf54f3b",
        remote = "https://github.com/google/woff2.git",
    )

    # HarfBuzz text shaping. Gated on //donner/svg/renderer:text_full_enabled.
    #
    # The patch_cmds below create src/config-override.h to re-enable parts of
    # the HarfBuzz API that HB_TINY/HB_LEAN strip out. We need:
    #   - the draw API for glyph outline extraction
    #   - CFF outlines for our OTF fallback font (Public Sans)
    #   - file I/O for hb_face_create_from_file_or_fail
    new_git_repository(
        name = "harfbuzz",
        build_file = "//third_party:BUILD.harfbuzz",
        remote = "https://github.com/harfbuzz/harfbuzz.git",
        tag = "14.1.0",
        patch_cmds = [
            """cat > src/config-override.h << 'HBEOF'
// Re-enable the draw API for glyph outline extraction.
// HB_TINY -> HB_LEAN -> HB_NO_DRAW -> HB_NO_OUTLINE, which strips these.
#undef HB_NO_DRAW
#undef HB_NO_OUTLINE
// Re-enable CFF outlines (our fallback font Public Sans is OTF/CFF).
#undef HB_NO_CFF
#undef HB_NO_OT_FONT_CFF
// Re-enable file I/O (hb_face_create_from_file_or_fail references
// hb_blob_create_from_file_or_fail which is guarded by HB_NO_OPEN).
#undef HB_NO_OPEN
HBEOF""",
        ],
    )

    # wgpu-native (Rust/wgpu-based WebGPU implementation) for the Geode GPU
    # renderer. Only fetched when
    # --//donner/svg/renderer/geode:enable_geode=true.
    #
    # Previous iteration built Dawn from source via rules_foreign_cc's cmake()
    # rule. On GitHub Actions that took ~1 h 45 m per run — too slow for the
    # interactive merge cycle. Swapping to a pre-built wgpu-native tarball
    # drops the "build Dawn" critical path to a ~12 MB download (single
    # shared library + two headers), measured in seconds.
    #
    # One http_archive per supported (os, cpu) tuple; the wgpu_native alias
    # in third_party/BUILD.wgpu_native picks the right one via `select()`.
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
        struct(
            name = "wgpu_native_macos_aarch64",
            asset = "wgpu-macos-aarch64-release.zip",
            sha256 = "f140ff27234ebfa9fcca2b492d0cb499f2e197424b9edc45134bcbad0f8d3a78",
        ),
        struct(
            name = "wgpu_native_macos_x86_64",
            asset = "wgpu-macos-x86_64-release.zip",
            sha256 = "1fbc6930e2811b7fde7f046e5300ae5dc20c451d0c3e42a10ff71efae1f565ac",
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
    new_git_repository(
        name = "tracy",
        build_file = "//third_party:BUILD.tracy",
        # Pinned to the latest stable upstream release. Bump deliberately.
        tag = "v0.12.2",
        remote = "https://github.com/wolfpld/tracy.git",
    )

    # resvg test suite: reference renderings used by image comparison tests.
    # Layout (post-2023-05 restructure + Great Rename): tests are under
    # tests/<category>/<feature>/<name>.svg with paired <name>.png in the
    # same directory. Resources (external images) under resources/, fonts
    # under fonts/.
    new_git_repository(
        name = "resvg-test-suite",
        build_file = "//third_party:BUILD.resvg-test-suite",
        commit = "d8e064337faf01bc5a9579187a56dbdbe3eacc72",
        remote = "https://github.com/linebender/resvg-test-suite.git",
    )

    # bazel_clang_tidy: --config=clang-tidy aspect (see .bazelrc).
    git_repository(
        name = "bazel_clang_tidy",
        commit = "c4d35e0d0b838309358e57a2efed831780f85cd0",
        remote = "https://github.com/erenon/bazel_clang_tidy.git",
    )

    # Hermetic Linux sysroots for Bazel remote execution via `--config=re`.
    #
    # The RE worker on bazel-re1 (NixOS) has no system glibc headers, so
    # libc++'s <wchar.h> can't find `mbstate_t`. Chromium's prebuilt
    # Debian Bullseye sysroots give the LLVM toolchain a complete set of
    # C/C++ headers + runtime stubs to link against, keeping the build
    # hermetic regardless of what the RE host does (or does not) install.
    #
    # Sourced from https://commondatastorage.googleapis.com/chrome-linux-sysroot
    # (URL + "/" + Sha256Sum; see Chromium's build/linux/sysroot_scripts/
    # install-sysroot.py). Bump by pulling the latest sysroots.json from
    # chromium/src and updating both the sha256 + URL fragment.
    # Exclude non-toolchain subtrees (docs, locale data, systemd units, etc.)
    # so the filegroup is both smaller and skips files whose names contain
    # characters Bazel rejects as target names — `lib/systemd/system/` ships
    # escape-sequence filenames like `system-systemd\x2dcryptsetup.slice`
    # that trip `glob()`.
    _SYSROOT_BUILD_FILE = """\
filegroup(
    name = "sysroot",
    srcs = glob(
        include = ["**"],
        exclude = [
            "etc/**",
            "lib/systemd/**",
            "usr/lib/systemd/**",
            "usr/share/doc/**",
            "usr/share/info/**",
            "usr/share/locale/**",
            "usr/share/man/**",
            "var/**",
            "**/* *",
        ],
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

non_bcr_deps = module_extension(
    implementation = _non_bcr_deps_impl,
)
