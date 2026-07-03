---
name: donner-release
description: >-
  Cutting a Donner release end-to-end: pre-release quality gates, RELEASE_NOTES.md rules, the
  dedicated build-report commit that gets tagged, GitHub Release + binary verification, Bazel
  Central Registry (BCR) publishing pre-flight, and dependency-update handling. Use when preparing
  or cutting a release, editing RELEASE_NOTES.md, running tools/generate_build_report.py,
  publishing to BCR (.bcr/, release.yml publish-to-bcr job), or handling Renovate/dependency PRs.
---

# Donner Release Engineering

Canonical docs — read these before improvising; this skill is the map, they are the territory:

| Doc                                     | What it holds                                                                                               |
| --------------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| `docs/release_checklist.md`             | The release checklist template. Work it top to bottom.                                                      |
| `docs/design_docs/0018-bcr_release.md`  | BCR publishing runbook + common-failures table.                                                             |
| `docs/design_docs/0011-v0_5_release.md` | v0.5 milestone plan; §"v0.5 Retrospective" lists release-process bugs carried forward.                      |
| `docs/design_docs/0028-v1_0_release.md` | v1.0 plan; Phase 1 tracks the BCR publish root-cause.                                                       |
| `docs/updating_dependencies.md`         | LLVM toolchain update flow (partially stale — see §Dependency updates).                                     |
| `docs/ProjectRoadmap.md`                | Roadmap source of truth; update post-release.                                                               |
| `docs/release_checklists/`              | Per-release manual checklists (currently `v0_8_showcase_checklist.md`, the showcase-asset authoring steps). |

0011 and 0028 are milestone plans; for execution only the cited sections matter (0011 §"v0.5
Retrospective" and §"Phase 2: Fuzzer Run", 0028 Phase 1) — skip the phase-by-phase implementation
logs.

`.claude/agents/ReleaseBot.md` encodes the same guardrails for the agent persona.

## Release sequence (order matters)

1. Pre-release quality gates (below) — all green.
2. Documentation audit (checklist §Pre-Release: Documentation).
3. Write the `RELEASE_NOTES.md` entry for the new version.
4. Bump `module(name = "donner", version = ...)` in `MODULE.bazel` to the release version (drop
   the `-pre` suffix). Required for **every** release, not just BCR publishes — a tag whose
   MODULE.bazel still says `-pre` is a defect. `docs/release_checklist.md` has no version-bump
   item (the step only appears as BCR pre-flight gate #1 in 0018); do it anyway.
5. **Last of all**: the dedicated build-report commit. It is the commit that gets tagged.
6. Tag, push tag, create the GitHub Release, verify binaries.
7. (If BCR publish is in scope) run the BCR pre-flight first — see §BCR publishing.
8. Post-release: update `docs/ProjectRoadmap.md` (mark milestone shipped), announce.

Any code fix discovered after the tag is a point-release concern. The tag never moves
retroactively — moving it breaks the GitHub source-tarball integrity hash that BCR records.

## Pre-release quality gates

From `docs/release_checklist.md` (each gate exists because a past release shipped without it):

- **Warning-clean build**: `bazel build //donner/...` with zero warnings. Warnings do NOT fail
  the build (no `-Werror` in `.bazelrc`) and Bazel does not re-emit warnings for cached actions,
  so an incremental build hides them. Verify with a full recompile:
  `bazel clean && bazel build //donner/... 2>&1 | grep -i warning` must print nothing.
- **Doxygen warning-free**: `doxygen Doxyfile 2>&1 | grep warning` prints nothing. Common causes:
  unescaped `@font-face` in comments (use backticks), broken `\ref` targets, undocumented public
  compounds. The gate was waived once, for v0.5, by explicit operator decision (0011
  §Retrospective) — treat it as a hard gate; only the operator can waive it, per release.
- **Tests green**: `bazel test //...` — the single validation command. The `donner_cc_test`
  variant wrappers (`*_tiny` / `*_text_full` / `*_geode`; `build_defs/rules.bzl`) make it cover
  the tiny / text-full / Geode lanes with no `--config=` flag. `docs/release_checklist.md` still
  says `bazel test //donner/...` plus `--config=text-full` — that predates the variant-lane
  consolidation; `bazel test //...` supersedes it.
- **Fuzzers run** with no new crashes. Precedent (v0.5, 0011 §"Phase 2: Fuzzer Run"): all 21
  fuzzers for 10 minutes each under `--config=asan-fuzzer` — see donner-fuzzing.
- **CMake build verified** — `python3 tools/cmake/gen_cmakelists.py --check --build` plus a CMake
  build/test pass. See donner-build-test.
- **Showcase asset loads and renders** (v0.8 onward): `//donner/editor/tests:showcase_asset_tests`
  (defined in `donner/editor/tests/BUILD.bazel`) fails if the checked-in showcase SVG is missing
  or invalid.
- **Experimental gates removed from shipped features**: delete
  `static constexpr bool IsExperimental = true` declarations entirely (absence is the
  non-experimental default — do not set to `false`).

## RELEASE_NOTES.md rules

- **Shipped entries are frozen history. Never retroactively edit them**, not even to "clarify".
  Historical notes record what was true at ship time; corrections and new information go in the
  next release's section. If asked to update an old entry, decline and add to the next one.
- You MAY draft/edit the section for the **unreleased** in-progress version (marked
  `*Date: <unreleased>*`) freely, including typo/link fixes.
- Entry shape (follow prior entries): high-level summary, categorized "What's Changed" bullets,
  breaking changes, "What's Included" build artifacts, a minimal usage code example, and a full
  changelog link (`compare/vOLD...vNEW`).

## The build-report commit invariant

The build report is regenerated as a **dedicated commit containing nothing else**, landing
**after** every other release-blocking change including the `RELEASE_NOTES.md` update. Rationale:
this commit is the one that gets tagged, so the report must describe exactly the tagged tree, and
mixing other changes in would invalidate CI evidence for them.

```sh
python3 tools/generate_build_report.py --all --save docs/build_report.md
```

- `--all` runs every section (coverage, tests, binary size, documentation link, public targets,
  external deps) — it is slow. For a fast partial while iterating: `--binary-size --public-targets`.
- The coverage section requires `lcov` and `genhtml` on `$PATH` (`brew install lcov` /
  `apt install lcov`) — install them before the release run, or the tool reports them missing.
- The run also refreshes `docs/reports/coverage.zip` (lcov HTML repacked as one archive) and
  `docs/reports/binary-size/`; commit those alongside `docs/build_report.md`
  (`tools/build_docs.sh` extracts them into the Doxygen site).
- Commit message pattern: `Release vX.Y.Z: regenerate build report`.
- Verify this commit is CI-green end-to-end before tagging it.
- If a license annotation is missing from the external-deps section, the repo-name join key is not
  matching — see `_normalize_external_dependency_repo`, `_PACKAGE_URL_FALLBACKS`, and
  `_PACKAGE_LICENSE_KIND_OVERRIDES` in `tools/generate_build_report.py`.

## Tag + GitHub Release

```sh
git tag -a vX.Y.Z -m "Donner SVG vX.Y.Z"        # on the build-report commit
git push origin vX.Y.Z
gh release create vX.Y.Z --title "Donner SVG vX.Y.Z" --notes-file release_body.md
```

- `release_body.md` is a scratch file: the new `RELEASE_NOTES.md` section body **without** the
  `## vX.Y.Z` heading (the `--title` flag supplies the title). It is not gitignored — write it
  under `/tmp` (or the scratchpad) or delete it before the next commit.
- Publishing the release triggers `.github/workflows/release.yml`, which builds
  `bazelisk build -c opt //donner/svg/tool:donner-svg` on Linux and macOS and attaches
  `donner-svg_linux_x86_64` and `donner-svg_darwin_arm64` to the release.
- Verify on the release page: correct tag, both binaries attached, body renders correctly.
- Existing tags (query, don't assume): `git tag -l 'v*'`.

## BCR publishing (Bazel Central Registry)

`docs/design_docs/0018-bcr_release.md` is the runbook — do not freelance. Current status per that
doc: **no successful BCR publish has ever landed**; the first attempt (v0.5.0 tag, 2026-04-16)
failed. **Mandatory before any retry**: pull up the previous attempt's BCR pull request
(`bazelbuild/bazel-central-registry`, `modules/donner/<prev>/`) and its `publish-to-bcr` workflow
run, identify exactly why it failed, and confirm the fix is in the tree. Never re-run the publish
blind — a blind retry burns another release tag on the same failure. Copy-pasteable queries:

```sh
gh pr list --repo bazelbuild/bazel-central-registry --search "donner in:title" --state all
gh run list --repo jwmcglynn/donner --workflow=release.yml  # publish-to-bcr is a release.yml job
gh run view <run-id> --repo jwmcglynn/donner --log-failed
```

Leading structural suspect (check this first): `release.yml`'s `linux`/`macos` jobs upload
binaries straight to the GitHub Release via `svenstaro/upload-release-action` and never publish
GitHub Actions _workflow artifacts_, while the pinned `publish-to-bcr` reusable workflow's
artifact-download step expects workflow artifacts (`release_files` / attestations) from the
calling run. Verify against the `publish.yaml` source at the exact pinned tag before fixing —
either produce the expected artifacts in the binary jobs or configure the reusable workflow's
inputs accordingly.

### How the pipeline works

GitHub Release published → `release.yml` job `publish-to-bcr` (runs only on
`release`/`published`, after the binary jobs) → calls the reusable workflow
`bazel-contrib/publish-to-bcr/.github/workflows/publish.yaml` pinned at an **exact tag**
(currently `@v1.4.1` — check `release.yml`; Publish-to-BCR publishes no floating major tags, so
`@v1` would never resolve). It reads the `.bcr/` templates (`config.yml`,
`metadata.template.json`, `source.template.json`, `presubmit.yml`), substitutes `{VERSION}`, and
opens a PR on `bazelbuild/bazel-central-registry` from the `jwmcglynn/bazel-central-registry`
fork. The `BCR_PUBLISH_TOKEN` secret must be a **Classic** personal access token with
`repo` + `workflow` scopes — fine-grained PATs cannot open PRs against public repos.

### What BCR consumers get

Tiny-skia renderer + text-base (`stb_truetype`) only. text-full (HarfBuzz/WOFF2) and the Geode
GPU backend are **not** BCR-consumable: their repos are fetched by the `dev_dependency = True`
module extension `third_party/bazel/non_bcr_deps.bzl`, which Bazel strips when Donner is consumed
as a `bazel_dep`. Any target reaching one of those hidden repos must be gated with
`target_compatible_with` on the relevant config_setting, or BCR presubmit breaks.

### Pre-flight gates (run all; each has bitten before)

1. **Version match**: `grep -n '^module(' MODULE.bazel` — the `version` must equal the tag being
   pushed (a `-pre` suffix like `0.8.0-pre` means the bump has not been stamped yet).
2. **No non-dev overrides**:
   ```sh
   grep -nE '^(git_repository|new_git_repository|git_override|archive_override)' MODULE.bazel
   ```
   Interpret matches, don't just count them: a `git_override` is acceptable **only if** the module
   it overrides is declared `bazel_dep(..., dev_dependency = True)` (consumers never resolve dev
   deps, and overrides in a non-root module are ignored anyway). A `git_override` on a
   **non-dev** dep is a release blocker: consumers would silently ignore the override and try to
   fetch a version (often `0.0.0`) that does not exist on BCR. The `use_repo_rule` import line
   also matches — ignore it.
3. **Consumer simulation cquery** (the most important gate): run the `bazel cquery` from
   0018 §"BCR-consumer simulation" over the BCR target allowlist and grep for
   `@skia|@harfbuzz|@woff2|@wgpu_native|@resvg-test-suite|@bazel_clang_tidy` source files. Must
   return zero matches; a match means a target on the allowlist needs `target_compatible_with`
   gating.
4. **Presubmit allowlist current**: `.bcr/presubmit.yml` `build_targets` must cover every new
   top-level public library added under `//donner` since the last release — new libraries are not
   picked up automatically.
5. **Tarball layout**: `.bcr/source.template.json` `strip_prefix` is `donner-{VERSION}` (GitHub
   archive convention: repo name + version).

Failure signature → fix table: 0018 §"Common failures & fixes" (integrity hash mismatch = tag
moved or tarball regenerated; publish-to-bcr skipped = missing/expired `BCR_PUBLISH_TOKEN`;
"fork not found" = fork missing).

## Dependency updates (Renovate + toolchains)

`renovate.json` (query it — these settings drift): schedule "on the 1st through 7th day of the
month"; PRs labeled `bot`; `ignorePaths: **/third_party/**` (vendored code is updated by hand);
digest pinning disabled for `com_google_absl` and `imgui`; `skia` rate-limited to a monthly
schedule. Treat Renovate PRs like any other PR: full `bazel test //...` before merge, and
cross-check the CMake mirror in `tools/cmake/gen_cmakelists.py`: deps in
`_MODULE_TO_FETCHCONTENT` pick up the MODULE.bazel version at generation time, but
`_HARDCODED_FETCHCONTENT` (currently absl, entt) pins versions by hand and Renovate never touches
that file — bump the pin there too, then run
`python3 tools/cmake/gen_cmakelists.py --check --build` (see donner-pr-ci, donner-build-test).

LLVM toolchain: `MODULE.bazel` currently declares
`bazel_dep(name = "toolchains_llvm", version = "1.8.0", dev_dependency = True)` — a plain BCR dep,
not a `git_override` (the `git_override` flow in `docs/updating_dependencies.md` is stale; the
jwmcglynn fork was used for an archived libclang experiment). To test local toolchain changes,
uncomment the `local_path_override(module_name = "toolchains_llvm", path = "../toolchains_llvm")`
block already sketched in `MODULE.bazel`, with the checkout as a sibling directory.

## Volatile facts — query, never trust memory

| Fact                        | Query                                                    |
| --------------------------- | -------------------------------------------------------- |
| Current module version      | `grep -n '^module(' MODULE.bazel`                        |
| Existing release tags       | `git tag -l 'v*'`                                        |
| Publish-to-BCR workflow pin | `grep -n 'publish-to-bcr' .github/workflows/release.yml` |
| Unreleased notes section    | `grep -n 'unreleased' RELEASE_NOTES.md`                  |
| BCR presubmit allowlist     | `grep -A40 'build_targets' .bcr/presubmit.yml`           |

Note that 0018's prose references an older workflow pin than `release.yml` actually uses — the
workflow file wins; the doc lags.

## Related skills

- donner-build-test — bazel configs (`text-full`, variants), CMake mirror gate.
- donner-pr-ci — CI lanes, merge policy (never merge without operator approval; squash only).
- donner-fuzzing — running the fuzz targets for the pre-release gate.
- donner-docs — Doxygen build, docs-site generation (`tools/build_docs.sh`).
- donner-embedding — what BCR/CMake consumers see; consumer-facing API surface.
