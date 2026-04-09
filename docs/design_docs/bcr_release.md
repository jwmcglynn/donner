# BCR Release Runbook

**Status:** Active — first BCR release is planned for v0.5.0.
**Last updated:** 2026-04-08.

This doc is the single source of truth for cutting a new Donner release on the [Bazel Central Registry](https://registry.bazel.build/). It's tuned for quick execution, not exhaustive explanation — the "why" lives in the companion docs and PRs linked at the bottom.

## TL;DR — the happy path

1. Bump `module(name = "donner", version = "X.Y.Z")` in `MODULE.bazel`
2. Run the pre-release checklist below, make sure it's green
3. Merge the release PR, tag `vX.Y.Z`, push
4. `.github/workflows/release.yml` builds CLI binaries + calls the Publish-to-BCR reusable workflow
5. The app opens a PR on `bazelbuild/bazel-central-registry` under `modules/donner/X.Y.Z/` from `jwmcglynn/bazel-central-registry` (a fork)
6. Watch BCR presubmit CI on that PR, iterate on `.bcr/presubmit.yml` if anything fails, ping a BCR maintainer, wait for merge

## What's on BCR (and what isn't)

Donner's BCR-published surface is a deliberate subset of the full library. The default build that BCR consumers get is **tiny-skia + text-base**:

| Feature | On BCR? | How BCR consumers get it |
|---|---|---|
| SVG parser, CSS, DOM, computed style | ✅ | default, no flags needed |
| Tiny-skia software renderer | ✅ | default backend |
| Text rendering via `stb_truetype` (text-base) | ✅ | default text tier |
| Filter effects (all 17 primitives) | ✅ | built-in |
| Skia backend (`--config=skia`) | ❌ | Power users must consume Donner via `git_override` |
| text-full (HarfBuzz + WOFF2) | ❌ | Power users via `git_override`; also tracked as a future follow-up BCR module |
| Geode / Dawn WebGPU backend | ❌ | Experimental; `git_override` only; revisit post-v0.5 |

The mechanism that keeps the non-BCR features invisible to BCR consumers is the `dev_dependency = True` module extension at `third_party/bazel/non_bcr_deps.bzl`. BCR strips dev-only extensions when Donner is consumed as a `bazel_dep`, so downstream users simply never see `@skia`, `@harfbuzz`, `@woff2`, or `@dawn`.

Every Donner target that references one of those hidden repos must be guarded by `target_compatible_with` on the relevant config_setting (e.g. `//donner/svg/renderer:text_full_enabled`, `//donner/svg/renderer:renderer_backend_skia`, `//donner/svg/renderer/geode:dawn_enabled`). If a BCR consumer's `bazel build @donner//...` ever tries to resolve one of those repos, the gating is broken — see the checklist below.

## Per-release checklist

Do these in order. Each step is either a command to run or a one-line visual check.

### Pre-flight
- [ ] Working tree on `main`, clean, up to date
- [ ] `docs/design_docs/v0_5_release.md` (or equivalent release doc) marks all release-blocking phases complete
- [ ] `RELEASE_NOTES.md` drafted for the version being cut

### Version bump
- [ ] `MODULE.bazel` → `module(name = "donner", version = "X.Y.Z")` matches the tag being pushed
- [ ] No stale references to the previous version in `docs/` or `README.md`

### Dev-config build matrix (local or CI)
- [ ] `bazel build //...` — default config (tiny-skia + text-base)
- [ ] `bazel build --config=skia //...` — full Skia backend still builds
- [ ] `bazel build --config=text-full //...` — HarfBuzz + WOFF2 text shaping still builds
- [ ] `bazel test //...` on at least the default config green

### BCR-consumer simulation (most important)
Simulate what a BCR downstream sees, where the `non_bcr_deps` dev extension is stripped and `@skia`/`@harfbuzz`/`@woff2`/`@dawn` do not exist.

- [ ] No `git_repository` / `new_git_repository` / non-dev `*_override` in top-level `MODULE.bazel` (grep for them):
      ```
      grep -nE '^(git_repository|new_git_repository|git_override|archive_override)' MODULE.bazel
      ```
      (Should return nothing. Dev-only entries live inside `use_extension(..., dev_dependency = True)` blocks or the extension .bzl file.)
- [ ] No non-BCR repo labels reachable from the BCR target allowlist in default config:
      ```
      bazel cquery 'kind("source file", deps( \
        //donner/base/... + //donner/css/... + //donner/svg:svg + \
        //donner/svg/parser/... + //donner/svg/components/... + \
        //donner/svg/core/... + //donner/svg/properties/... + \
        //donner/svg/graph/... + //donner/svg/resources/... + \
        //donner/svg/renderer:rendering_context + \
        //donner/svg/renderer:renderer_tiny_skia))' \
        | grep -E '^@(skia|harfbuzz|woff2|dawn|resvg-test-suite|bazel_clang_tidy)'
      ```
      Must return **zero** matches. If there are matches, a target in that allowlist has a dep that needs `target_compatible_with` gating (or a new target was added and isn't on the `.bcr/presubmit.yml` allowlist).
- [ ] `.bcr/presubmit.yml` `build_targets` cover every top-level library consumers might reasonably want. Add new libs here when you add them under `//donner`.
- [ ] `.bcr/source.template.json` `strip_prefix` matches `donner-{VERSION}` (GitHub tarball convention).

### Scaffolding sanity
- [ ] `.bcr/config.yml`, `.bcr/metadata.template.json`, `.bcr/source.template.json`, `.bcr/presubmit.yml` all valid YAML/JSON
- [ ] `.github/workflows/release.yml` references a real `bazel-contrib/publish-to-bcr/.github/workflows/publish.yaml@vX.Y.Z` tag (Publish-to-BCR does not publish floating major tags; pin exact versions)

### Ship
- [ ] Merge release PR → push tag `vX.Y.Z` → GitHub Release auto-created
- [ ] Watch Actions tab: `linux` + `macos` CLI binary jobs run, then `publish-to-bcr` reusable workflow
- [ ] Watch `bazelbuild/bazel-central-registry` → `modules/donner/X.Y.Z/` for the new PR
- [ ] Iterate on BCR presubmit failures via the common-failures table below
- [ ] Ping a BCR maintainer in the PR comments when presubmit CI goes green
- [ ] After BCR merge: `https://registry.bazel.build/modules/donner` shows the new version

## Publish-to-BCR flow details

The
[Publish-to-BCR reusable GitHub Actions workflow](https://github.com/bazel-contrib/publish-to-bcr)
handles the mechanical pieces:

1. On GitHub Release publish, `release.yml` invokes `bazel-contrib/publish-to-bcr/.github/workflows/publish.yaml@v1.2.0` with `tag_name: ${{ github.event.release.tag_name }}`, `registry_fork: jwmcglynn/bazel-central-registry`, and `secrets.publish_token = secrets.BCR_PUBLISH_TOKEN`. The calling job must also set `id-token: write`, `attestations: write`, and `contents: write` permissions.
2. The reusable workflow pulls the release tarball, computes the SHA256 integrity, reads `.bcr/metadata.template.json` + `.bcr/source.template.json` + `.bcr/presubmit.yml`, substitutes `{VERSION}` and `{TAG}` placeholders, and writes a new `modules/donner/X.Y.Z/` entry in the maintainer's BCR fork.
3. It opens a PR from `jwmcglynn/bazel-central-registry` to
   `bazelbuild/bazel-central-registry` on the maintainer's behalf.

This flow does **not** use the legacy Publish-to-BCR GitHub App. Upstream marks
that app as legacy and says it will be discontinued after **June 30, 2026**, so
new Donner releases should stay on the reusable-workflow path. The only
one-time setup for this workflow is a BCR fork plus a Classic PAT (or a machine
user PAT) with `repo` + `workflow` scopes.

Bump the reusable workflow pin (`@v1.2.0`) when new releases of Publish-to-BCR land — see [the releases page](https://github.com/bazel-contrib/publish-to-bcr/releases). Prefer exact version tags over floating refs like `@v1`, because Publish-to-BCR does not publish floating major-version tags.

### One-time Publish-to-BCR setup
1. Fork `bazelbuild/bazel-central-registry` to
   `jwmcglynn/bazel-central-registry`.
2. Create a **Classic** PAT with `repo` + `workflow` scopes. Add it as
   `BCR_PUBLISH_TOKEN` in the `jwmcglynn/donner` repo secrets. Fine-grained
   PATs are not currently supported for opening PRs against public repos; see
   [github/roadmap#600](https://github.com/github/roadmap/issues/600).
3. If Donner releases move to an org-owned or shared-maintainer model, prefer a
   dedicated machine user PAT over a maintainer's personal token.

### Where to watch
- Release workflow logs: `https://github.com/jwmcglynn/donner/actions` (Release workflow)
- BCR PR: `https://github.com/bazelbuild/bazel-central-registry/pulls?q=is:pr+author:jwmcglynn+donner`
- Registry entry: `https://registry.bazel.build/modules/donner`

## Common failures & fixes

Update this section with real-world lessons as they happen.

| Symptom | Cause | Fix |
|---|---|---|
| BCR PR presubmit: `Unmapped external dep: @skia//:core` | A target referenced `@skia` but wasn't gated by `target_compatible_with` | Add `target_compatible_with = select({...renderer_backend_skia: [], //conditions:default: ["@platforms//:incompatible"]})` to the offending target (or use `renderer_backend_compatible_with(["skia"])`) |
| BCR PR presubmit: target not found | New top-level library added under `//donner` since last release | Add it to `.bcr/presubmit.yml` `build_targets` |
| BCR PR presubmit: integrity hash mismatch | GitHub regenerated the source tarball or the tag moved | Re-upload the release tarball verbatim; never force-push tags |
| `source.template.json` URL 404 | `strip_prefix` doesn't match GitHub's tarball layout | Confirm pattern is `donner-{VERSION}` (GitHub uses repo name + version) |
| Release workflow: `publish-to-bcr` skipped | `BCR_PUBLISH_TOKEN` secret missing or PAT expired | Regenerate PAT, re-save secret |
| Release workflow: `publish-to-bcr` fails with "fork not found" | Maintainer BCR fork hasn't been created yet | Fork `bazelbuild/bazel-central-registry` to the `registry_fork` configured in `release.yml` |

## Adding a new top-level library

When you create a new top-level library under `//donner/...`, the BCR presubmit allowlist won't know about it automatically.

1. Add `"@donner//donner/your/new/package/..."` (or the specific target) to `.bcr/presubmit.yml` `build_targets`.
2. Re-run the BCR-consumer simulation `cquery` above to confirm your new library doesn't transitively pull in any non-BCR dep.
3. If it does (and that's intentional — e.g. it's text-full-specific), gate the offending target with `target_compatible_with` on the relevant config_setting, same as `text_backend_full` and `woff2_parser`.

## Future BCR scope expansion

Things that are deliberately out of scope for the first few BCR releases but may land later:

- **text-full on BCR** — vendor HarfBuzz + WOFF2 via `git subtree` (~1–2 days of `BUILD.harfbuzz` work), or ship a sibling `donner-text-full` module that layers on top of `donner` and brings its own HB/WOFF2. Blocked on: deciding whether to own an additional BCR module or vendor.
- **Separate `tiny-skia-cpp` BCR module** — it already has its own `MODULE.bazel` in `third_party/tiny-skia-cpp`; could be published independently and then consumed as a BCR `bazel_dep` from Donner. Blocked on: deciding the dev vs publish trade-off.
- **Geode / Dawn on BCR** — Dawn is not publishable on BCR (too large, Chromium cadence). Track upstream progress, revisit for a `donner-geode` module post-v1.0.
- **Skia backend on BCR** — not realistic. Skia is a monorepo with a custom build. It will stay `git_override`-only for the foreseeable future.

## References

- [Publish-to-BCR](https://github.com/bazel-contrib/publish-to-bcr) — the reusable workflow this runbook drives
- [bazelbuild/bazel-central-registry](https://github.com/bazelbuild/bazel-central-registry) — the BCR repository
- [rules_foreign_cc/.bcr/](https://github.com/bazelbuild/rules_foreign_cc/tree/main/.bcr) — reference `.bcr/` layout for a C++ library
- `docs/design_docs/v0_5_release.md` — v0.5 release scope
- `third_party/bazel/non_bcr_deps.bzl` — the dev-only extension that hides non-BCR deps
- `docs/release_checklist.md` — generic release checklist (pairs with this BCR-specific runbook)
