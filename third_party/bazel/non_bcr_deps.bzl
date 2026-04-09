"""Module extension for non-BCR dependencies.

These repos are fetched only when donner is the root module (i.e. local
development or CI), not when donner is consumed as a dependency from the
Bazel Central Registry. The extension is imported from //MODULE.bazel with
`dev_dependency = True`, so downstream BCR consumers never see these repos.

Each dep listed here is load-bearing for a feature that is opt-in and NOT
shipped over BCR:

- skia            : Skia renderer backend       (--config=skia)
- harfbuzz        : Text shaping                (--config=text-full)
- woff2           : WOFF2 font format           (--config=text-full)
- dawn            : WebGPU (Geode renderer)     (--//donner/svg/renderer/geode:enable_dawn=true)
- resvg-test-suite: Reference SVG goldens       (image comparison tests)
- bazel_clang_tidy: clang-tidy aspect           (--config=clang-tidy)

When / how to add a new non-BCR dep:
  1. If the dep is load-bearing for the default tiny-skia + text-base build,
     it MUST NOT live here — it must either be on BCR (as a bazel_dep), or
     vendored under third_party/ via `new_local_repository` so it ships in
     the source archive.
  2. If the dep is gated behind a config_setting / bool_flag AND every BUILD
     target referencing it has `target_compatible_with = <flag>_enabled`,
     add it to `_non_bcr_deps_impl` below.
  3. Update the checklist in docs/design_docs/bcr_release.md (when that
     document exists — tracked under the BCR release plan).

See docs/design_docs/bcr_release.md for the fuller picture of what ships on
BCR versus what stays behind `git_override`.
"""

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository", "new_git_repository")

def _non_bcr_deps_impl(_mctx):
    # Skia renderer backend. Gated on //donner/svg/renderer:renderer_backend_skia.
    git_repository(
        name = "skia",
        commit = "d945cbcbbb5834245256e883803c2704f3a32e18",
        remote = "https://github.com/google/skia",
    )

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

    # Dawn (WebGPU implementation) for the Geode GPU renderer.
    # Only fetched when --//donner/svg/renderer/geode:enable_dawn=true.
    # `patch_cmds` runs `fetch_dawn_dependencies.py` at repository fetch time
    # (which has network access) to populate third_party/ submodules, so the
    # downstream CMake build can run with DAWN_FETCH_DEPENDENCIES=OFF inside
    # Bazel's sandbox.
    new_git_repository(
        name = "dawn",
        build_file = "//third_party:BUILD.dawn",
        # Pinned to a specific commit for reproducibility. Bump deliberately.
        # From v20260403.135149 (2026-04-03).
        commit = "fa93dacdf3931ec29ff8f42facf51db632c45308",
        remote = "https://dawn.googlesource.com/dawn",
        patch_cmds = [
            "python3 tools/fetch_dawn_dependencies.py",
            # Strip nested .git directories so Bazel's file tracking sees the
            # submodule contents as regular source files.
            "find third_party -name .git -type d -exec rm -rf {} + 2>/dev/null || true",
            # Strip nested BUILD.bazel/BUILD/WORKSPACE files throughout the tree.
            # Otherwise they create Bazel package boundaries and glob(['**']) stops
            # recursing, leaving CMake unable to find many source subdirectories.
            # Dawn has ~114 BUILD.bazel files under src/tint alone (Tint's upstream
            # Bazel support for WGSL parsing), plus more in third_party submodules.
            # The root BUILD file is provided by our overlay so the top-level one
            # is not needed.
            "find . -mindepth 2 \\( -name BUILD.bazel -o -name BUILD -o -name WORKSPACE -o -name WORKSPACE.bazel -o -name MODULE.bazel \\) -type f -delete 2>/dev/null || true",
            # Remove the top-level WORKSPACE/MODULE files that Dawn ships with.
            # (Keep our BUILD.bazel overlay which is placed at the root by Bazel's
            # new_git_repository build_file attribute — don't remove that one.)
            "rm -f WORKSPACE WORKSPACE.bazel MODULE.bazel",
        ],
    )

    # resvg test suite: reference renderings used by image comparison tests.
    new_git_repository(
        name = "resvg-test-suite",
        build_file = "//third_party:BUILD.resvg-test-suite",
        commit = "d8e064337faf01bc5a9579187a56dbdbe3eacc72",
        remote = "https://github.com/RazrFalcon/resvg-test-suite.git",
    )

    # bazel_clang_tidy: --config=clang-tidy aspect (see .bazelrc).
    git_repository(
        name = "bazel_clang_tidy",
        commit = "c4d35e0d0b838309358e57a2efed831780f85cd0",
        remote = "https://github.com/erenon/bazel_clang_tidy.git",
    )

non_bcr_deps = module_extension(
    implementation = _non_bcr_deps_impl,
)
