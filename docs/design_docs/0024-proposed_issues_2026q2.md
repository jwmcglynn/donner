# Design: Proposed Issues — 2026 Q2 Feature Gaps, Bugs & Build Improvements

**Status:** Draft
**Author:** Claude Opus 4.6 (with SpecBot, PerfBot, BazelBot)
**Created:** 2026-04-12

## Summary

Comprehensive audit of the resvg test suite, rendering pipeline, and build infrastructure to identify
actionable GitHub issues. This doc catalogs **~307 skipped tests** (plus ~51 render-only) across
the resvg suite, distills them into **24 SVG feature/bug issues**, and proposes **14 CI/build
optimization issues** for reducing CI runtime and binary size.

**Key findings:**

- **P0 quick wins** (2 issues) would unblock **50 tests** with small effort — CSS filter functions
  and `transform-origin` are already parsed but not applied at render time.
- **P0+P1 combined** (7 issues) would unblock **~139 tests** — roughly 60% of all failures.
- **~30 tests** are intentionally skipped (deprecated SVG 1.1 features) and should stay skipped.
- **CI wall-clock time** is dominated by macOS runner queue wait (55–130 min); actual build+test
  work is only ~12 min (Linux) / ~9 min (macOS).
- **Compilation speed** is bottlenecked by the monolithic `entt/entt.hpp` header (95,903 lines)
  included transitively by 48 source files.

## Goals

- Provide a prioritized backlog of issues that can be filed on GitHub.
- Each issue has clear scope, test coverage impact, spec references, and complexity estimate.
- Enable data-driven prioritization of v0.5+ work.
- Identify quick wins that unblock the most resvg tests with the least effort.

## Non-Goals

- Actually filing the issues (this doc is the proposal for review).
- Implementing any of the proposed changes.
- Covering Geode backend gaps (tracked separately in the Geode design doc).
- Animation or scripting support (out of scope for current milestone).

## Next Steps

1. Review this doc and decide which issues to file.
2. File P0 issues first — they're small, high-impact, and have no dependencies.
3. File P1 issues and assign to upcoming sprints.

---

# Part 1: SVG Feature Gaps & Bugs

Source data: `donner/svg/renderer/tests/resvg_test_suite.cc` (1,539 lines), 307 `Params::Skip()`
entries, 51 `Params::RenderOnly()` entries, 9 elevated-threshold entries.

## Intentionally Skipped — Do Not File (30 tests)

These are deprecated SVG 1.1 features. They should remain `Params::Skip()` permanently.

| Feature | Tests | Spec Status |
|---------|-------|-------------|
| `enable-background` | 17 | Removed in SVG2 §11.7.7 |
| `<tref>` | 9 | Removed in SVG2 §11.5 |
| `kerning` attribute | 2 | Deprecated; replaced by `font-kerning` |
| `glyph-orientation-*` | 2 | Deprecated; replaced by `text-orientation` |

---

## P0 — Quick Wins (50 tests, small complexity)

These features are **already parsed** — the rendering path just needs to be connected.

### Issue S1: Apply CSS filter function shorthands at render time

| Field | Value |
|-------|-------|
| **Tests unblocked** | 30 (`filters/filter-functions/*`) |
| **Complexity** | Small — this is a **bug**, not a new feature |
| **Spec** | [Filter Effects §8 — Filter Functions](https://www.w3.org/TR/filter-effects-1/#FilterFunction) |
| **Already tracked** | B2 in `docs/design_docs/0021-resvg_feature_gaps.md` |

**Current state:** `PropertyRegistry::ParseFilterFunction()` correctly parses all 10 CSS filter
functions (`blur()`, `brightness()`, `contrast()`, `drop-shadow()`, `grayscale()`, `hue-rotate()`,
`invert()`, `opacity()`, `saturate()`, `sepia()`). The rendering path only recognizes
`ElementReference` variants (SVG `url(#id)` form) and ignores the CSS function vector.

**Key files:** `RenderingContext.cc:244`, `RendererDriver.cc`
**Dependencies:** None

### Issue S2: Support `transform-origin` as a presentation attribute

| Field | Value |
|-------|-------|
| **Tests unblocked** | 20 (`structure/transform-origin/*`) |
| **Complexity** | Small–Medium |
| **Spec** | [CSS Transforms 2 §6](https://www.w3.org/TR/css-transforms-2/#transform-origin-property), [SVG2 §5.1](https://www.w3.org/TR/SVG2/styling.html#PresentationAttributes) |
| **Already tracked** | F2 in `docs/design_docs/0021-resvg_feature_gaps.md` |

**Current state:** Works in `style=""` attribute. `LayoutSystem.cc` computes and applies it
correctly. The presentation-attribute parsing path (`<rect transform-origin="50% 50%">`) does not
recognize it. Single-keyword syntax (`center`) doesn't parse either.

**Key files:** `PropertyRegistry.cc`, `TransformOrigin.h`
**Dependencies:** None

---

## P1 — High Impact (89 tests, medium complexity)

### Issue S3: Fix `<image>` element sizing and layout

| Field | Value |
|-------|-------|
| **Tests unblocked** | 18 (`structure/image/*`) |
| **Complexity** | Medium — multiple sub-bugs |
| **Spec** | [SVG2 §5.8](https://www.w3.org/TR/SVG2/embedded.html#ImageElement), [CSS Images 3 §5](https://www.w3.org/TR/css-images-3/#sizing) |

Embedded data-URL images render but at wrong size. `preserveAspectRatio` modes need investigation.
5 of 18 tests involve `<use>` referencing inline `<svg>` elements with width/height/viewBox
combinations.

**Partially blocked by:** B1 (non-square viewBox + percent resolution)
**Key files:** `LayoutSystem.cc`, `SizedElementComponent.h`

### Issue S4: Implement `<switch>` element + `systemLanguage`

| Field | Value |
|-------|-------|
| **Tests unblocked** | 15 (`structure/switch/*` + `structure/systemLanguage/*`) |
| **Complexity** | Medium |
| **Spec** | [SVG2 §5.9 — Conditional Processing](https://www.w3.org/TR/SVG2/struct.html#ConditionalProcessing) |

Not implemented — no `SVGSwitchElement` in `ElementType.h`. For a standalone renderer, the language
should be configurable (default: `en`). `systemLanguage` uses BCP 47 prefix matching.

**Dependencies:** None
**Key files:** New `SVGSwitchElement.h/.cc`, `ElementType.h`

### Issue S5: Implement `dominant-baseline` full keyword set

| Field | Value |
|-------|-------|
| **Tests unblocked** | 14 (`text/dominant-baseline/*`) |
| **Complexity** | Small — adding 4 mapped keywords |
| **Spec** | [CSS Inline 3 §4.2](https://www.w3.org/TR/css-inline-3/#dominant-baseline-property) |

9 of 13 keywords work. Missing: `before-edge` → `text-top`, `after-edge` → `text-bottom`,
`text-before-edge` → `text-top`, `text-after-edge` → `text-bottom` (SVG 1.1 legacy mappings).

**Key files:** `PropertyRegistry.cc:198-215`, `TextEngine.cc:311-325`

### Issue S6: Implement `context-fill` / `context-stroke` rendering

| Field | Value |
|-------|-------|
| **Tests unblocked** | 12 (`painting/context/*`) |
| **Complexity** | Medium–Large |
| **Spec** | [SVG2 §13.3 — Context paint](https://www.w3.org/TR/SVG2/painting.html#SpecifyingPaint) |

Parsed (`PaintServer::ContextFill`/`ContextStroke` variants exist). The marker rendering path does
not propagate context paint. Heavily used by icon libraries.

**Key files:** `RenderingContext.cc`, `PaintServer.h`

### Issue S7: Implement `alignment-baseline` full keyword set

| Field | Value |
|-------|-------|
| **Tests unblocked** | 10 (`text/alignment-baseline/*`) |
| **Complexity** | Small–Medium |
| **Spec** | [CSS Inline 3 §4.4](https://www.w3.org/TR/css-inline-3/#alignment-baseline-property) |

Missing `baseline` (the initial value per spec), `no-change`, `reset-size`, `use-script`.

**Dependencies:** Issue S5 (shared enum expansion)
**Key files:** `PropertyRegistry.cc:1522`

---

## P2 — Medium Impact (38 tests, medium complexity)

### Issue S8: Implement `paint-order` property rendering

| Field | Value |
|-------|-------|
| **Tests unblocked** | 8 (`painting/paint-order/*`) |
| **Complexity** | Medium |
| **Spec** | [SVG2 §13.11](https://www.w3.org/TR/SVG2/painting.html#PaintOrder) |

Parsed as an unparsed CSS string, not applied at render time. Controls fill/stroke/markers paint
order. Commonly used for "stroke behind fill" text effects.

**Key files:** All three renderers, `PropertyRegistry.cc`

### Issue S9: Fix `mask-type` property + mask edge cases

| Field | Value |
|-------|-------|
| **Tests unblocked** | 6 (`masking/mask/*`) |
| **Complexity** | Medium |
| **Spec** | [CSS Masking 1 §6.1](https://www.w3.org/TR/css-masking-1/#the-mask-type) |

Property not defined in PropertyRegistry. Sub-issues: `mask-type=alpha`, `mask-type` in style,
mask color-interpolation in linearRGB, `mask-on-self`, mask-units edge cases.

**Key files:** `RendererDriver.cc` (mask compositing), `PropertyRegistry.cc`

### Issue S10: Fix `feMorphology` edge cases

| Field | Value |
|-------|-------|
| **Tests unblocked** | 5 (`filters/feMorphology/*`) |
| **Complexity** | Small |
| **Spec** | [Filter Effects 1 §15](https://www.w3.org/TR/filter-effects-1/#feMorphologyElement) |

Core erode/dilate works. Failures on boundary conditions: empty radius, non-numeric radius,
zero radius (should be passthrough), negative radius.

**Key files:** `FilterSystem.cc`, `FilterGraphExecutor.cc:483-497`

### Issue S11: Fix `<use>` referencing inline `<svg>` edge cases

| Field | Value |
|-------|-------|
| **Tests unblocked** | 5 (`structure/use/*`) |
| **Complexity** | Medium |
| **Spec** | [SVG2 §5.6](https://www.w3.org/TR/SVG2/struct.html#UseElement) |

Shadow tree instantiation edge cases with `<use>` referencing inline `<svg>` elements with various
width/height/viewBox combinations.

**Dependencies:** Issue S3 (shared viewport resolution logic)
**Key files:** `SVGUseElement.cc`, `ShadowTreeSystem.cc`

### Issue S12: Fix intrinsic sizing + percent resolution

| Field | Value |
|-------|-------|
| **Tests unblocked** | 5 (across 3 categories) |
| **Complexity** | Medium |

For SVGs with non-square `viewBox` and no explicit `width`/`height`, Donner's intrinsic document
size comes out wrong. Already tracked as B1 in `0021-resvg_feature_gaps.md`.

**Key files:** `LayoutSystem.cc`, `SizedElementComponent.h`

### Issue S13: Fix `stroke-dasharray` edge cases

| Field | Value |
|-------|-------|
| **Tests unblocked** | 4 (`painting/stroke-dasharray/*`) |
| **Complexity** | Small |
| **Spec** | [SVG2 §13.5.6](https://www.w3.org/TR/SVG2/painting.html#StrokeDasharrayProperty) |

Edge cases: odd-length arrays (must be doubled per spec), zero-length dashes, all-zero sums
(treat as solid line).

### Issue S14: Fix `<filter>` edge cases (filterRes, filterUnits, multiple inputs)

| Field | Value |
|-------|-------|
| **Tests unblocked** | 4 (`filters/filter/*`) |
| **Complexity** | Medium |

Skipped tests: `content-outside-the-canvas-2`, `in=BackgroundAlpha`, `with-mask-on-parent`,
`with-transform-outside-of-canvas`.

### Issue S15: Fix text rendering edge cases (mixed inline, BiDi-adjacent)

| Field | Value |
|-------|-------|
| **Tests unblocked** | 4 (`text/text/*`) |
| **Complexity** | Medium |

Filter bbox, ligatures in mixed fonts, percent values on dx/dy, real text height.

### Issue S16: Implement `clipPath` with `<text>` children

| Field | Value |
|-------|-------|
| **Tests unblocked** | 4–7 (`masking/clipPath/*`) |
| **Complexity** | Medium–Large |
| **Spec** | [CSS Masking 1 §7.2](https://www.w3.org/TR/css-masking-1/#the-clip-path) |

ClipPath works for shapes but text children are not supported.

**Dependencies:** Text rendering pipeline

### Issue S17: Fix `feImage` edge cases

| Field | Value |
|-------|-------|
| **Tests unblocked** | 3 (`filters/feImage/*`) |
| **Complexity** | Small–Medium |

Subregion with rotation, x/y with protruding subregions. Also `feImage` with external href
(separate from ResourceLoader story).

### Issue S18: Implement `<a>` element rendering

| Field | Value |
|-------|-------|
| **Tests unblocked** | 3 (`structure/a/*`) |
| **Complexity** | Small — treat as `<g>` for rendering |
| **Spec** | [SVG2 §5.4](https://www.w3.org/TR/SVG2/linking.html#AElement) |

Not implemented. For rendering purposes `<a>` just needs to be a grouping element.

---

## P3 — Lower Impact / Higher Complexity

### Issue S19: Implement `textLength` + `lengthAdjust` (text stretching)

| Field | Value |
|-------|-------|
| **Tests unblocked** | 4 (`text/textLength/*`) + 3 (`text/lengthAdjust/*`) = 7 |
| **Spec** | [SVG2 §11.6.2](https://www.w3.org/TR/SVG2/text.html#TextElementTextLengthAttribute) |

### Issue S20: Implement textPath SVG2 features

| Field | Value |
|-------|-------|
| **Tests unblocked** | 5+ (`text/textPath/*`) |
| **Complexity** | Medium–Large per sub-feature |
| **Spec** | [SVG2 §11.7](https://www.w3.org/TR/SVG2/text.html#TextLayoutPath) |

Sub-features: `method="stretch"`, `spacing="auto"`, `side="right"`, `path` attribute (inline path
data), referencing `<rect>`/`<circle>`.

### Issue S21: Implement BiDi text (`direction` + `unicode-bidi`)

| Field | Value |
|-------|-------|
| **Tests unblocked** | 5+ (across `text/direction`, `text/unicode-bidi`, RTL tests) |
| **Complexity** | **Large** — requires Unicode Bidirectional Algorithm |
| **Spec** | [CSS Writing Modes 4 §2](https://www.w3.org/TR/css-writing-modes-4/#text-direction), [UAX #9](https://unicode.org/reports/tr9/) |

Properties registered but ignored. Consider scoping as "basic LTR/RTL" first.

### Issue S22: Implement `text-decoration` SVG2 decomposition

| Field | Value |
|-------|-------|
| **Tests unblocked** | 2 (`text/text-decoration/*`) |
| **Spec** | [CSS Text Decoration 3](https://www.w3.org/TR/css-text-decor-3/) |

Basic underline/overline/line-through works. Independent color/style not supported.

### Issue S23: Implement `font-kerning` property

| Field | Value |
|-------|-------|
| **Tests unblocked** | 2 (`text/font-kerning/*`) |
| **Complexity** | Small (for `--config=text-full`) |

Would toggle HarfBuzz's `kern` feature.

### Issue S24: Implement `image-rendering` full value set

| Field | Value |
|-------|-------|
| **Tests unblocked** | 2 (`painting/image-rendering/*`) |
| **Complexity** | Small |
| **Spec** | [CSS Images 3 §5.3](https://drafts.csswg.org/css-images-3/#the-image-rendering) |

Only `pixelated` → nearest-neighbor is implemented. Missing `crisp-edges`, `smooth`/`auto`.

---

## SVG Feature Impact Summary

| Priority | Issues | Tests Unblocked | Cumulative |
|----------|--------|-----------------|------------|
| **P0** | S1, S2 | 50 | 50 |
| **P1** | S3–S7 | 69 | 119 |
| **P2** | S8–S18 | ~51 | ~170 |
| **P3** | S19–S24 | ~23 | ~193 |
| Intentional skips | — | 30 | — |

## Recommended Implementation Order

The dependency graph suggests this phasing:

```
Phase 1 (P0 — immediate):
  S1 CSS filter functions ──→ +30 tests
  S2 transform-origin attr ──→ +20 tests

Phase 2 (P1 — text baselines):
  S5 dominant-baseline ──→ +14 tests
  S7 alignment-baseline ──→ +10 tests (depends on S5)

Phase 3 (P1 — elements):
  S4 <switch> + systemLanguage ──→ +15 tests
  S18 <a> element ──→ +3 tests (trivial, bundle with S4)

Phase 4 (P1 — rendering features):
  S6 context-fill/stroke ──→ +12 tests
  S8 paint-order ──→ +8 tests

Phase 5 (P1-P2 — image/viewport):
  S12 intrinsic sizing ──→ +5 tests (unblocks S3, S11)
  S3 <image> sizing ──→ +18 tests
  S11 <use> edge cases ──→ +5 tests

Phase 6 (P2 — small fixes):
  S10 feMorphology edges ──→ +5 tests
  S13 stroke-dasharray edges ──→ +4 tests
  S9 mask-type ──→ +6 tests

Phase 7+ (P3 — as prioritized):
  S14–S17, S19–S24
```

---

# Part 2: CI Runtime & Binary Size Improvements

Source data: CI workflow YAMLs, `.bazelrc`, `build_defs/rules.bzl`, `MODULE.bazel`, measured CI run
times from run `24295860066`.

## Current CI State

| Job | Platform | Queue Wait | Actual Work | Total |
|-----|----------|-----------|-------------|-------|
| Linux | ubuntu-24.04 | ~0s | ~12 min | ~12 min |
| Linux-Geode | ubuntu-24.04 | ~0s | ~2 min | ~2 min |
| macOS | macos-15 | **55–130 min** | ~9 min | 65–140 min |
| CMake (Linux) | ubuntu-24.04 | ~0s | ~10 min | ~10 min |
| CMake (macOS) | macos-15 | varies | ~10 min | varies |
| Coverage | ubuntu-24.04 | ~0s | ~30–45 min | ~30–45 min |
| CodeQL | ubuntu-latest | ~0s | ~10–60 min | varies |

**End-to-end CI: 65–191 min**, dominated by macOS runner queue wait.

---

## P0 — Critical

### Issue B1: Reduce macOS runner queue latency

| Field | Value |
|-------|-------|
| **Impact** | −60 to −120 min per CI run |
| **Effort** | 1–2 hours |

The single largest CI bottleneck is macOS runner queue wait (55–130 min), not build/test time.
Actual macOS work is only ~9 min.

**Options:**
- Use `macos-latest` (macOS 14) if the runner pool is larger.
- Run macOS CI only on `main` merges, use Linux as the primary PR gate.
- Defer fuzzer tests (2.3 min, separate LLVM 21 toolchain) to nightly/main-only.
- Consider self-hosted macOS runners.

**Key files:** `.github/workflows/main.yml`

---

## P1 — High Impact

### Issue B2: Enable Bazel remote cache for CI

| Field | Value |
|-------|-------|
| **Impact** | −30–50% build time on cache-hit runs |
| **Effort** | 2–4 hours |

No remote cache is configured. Every CI run rebuilds from scratch on cache miss.

**Options:** GitHub Actions Cache via `bazel-contrib/setup-bazel`, self-hosted `bazel-remote`,
or Google's free RBE tier for open-source.

**Key flags:** `--remote_cache=<url> --remote_upload_local_results --remote_timeout=300`
**Key files:** `.bazelrc`, `.github/workflows/main.yml`

### Issue B3: Increase `--local_test_jobs` from 2 to `HOST_CPUS`

| Field | Value |
|-------|-------|
| **Impact** | −20–40% test execution time on Linux |
| **Effort** | 5 min (one-line change) |

Linux CI artificially limits test parallelism to 2 despite 4-core runners. The test step is
7.8 min — the dominant cost of the Linux job. No tests use `local = True` or `tags = ["exclusive"]`.

**Key files:** `.github/workflows/main.yml:75,125`

### Issue B4: Switch from monolithic `entt/entt.hpp` to modular EnTT headers

| Field | Value |
|-------|-------|
| **Impact** | −30–50% compile time for ECS-touching TUs |
| **Effort** | 2–3 days |

`EcsRegistry.h` includes the monolithic 95,903-line single-include header. 48 non-test files
transitively include it. Donner only uses `entt::entity`, `entt::basic_registry`,
`entt::basic_handle`. The modular headers (73 files) are already vendored.

**Replace with:**
```cpp
#include <entt/entity/fwd.hpp>      // 290 lines
#include <entt/entity/registry.hpp>  // 1,239 lines
#include <entt/entity/handle.hpp>    // 431 lines
```

Also create `EcsRegistry_fwd.h` for headers that only need type names.

**Key files:** `donner/base/EcsRegistry.h`, `donner/svg/AllSVGElements.h`

### Issue B5: Add `--config=ci` with CI-specific optimizations

| Field | Value |
|-------|-------|
| **Impact** | Maintainability + tuning |
| **Effort** | 30 min |

CI-specific flags are scattered across workflow YAML. Centralizing in `.bazelrc` as `--config=ci`
makes them testable locally.

```
build:ci --local_test_jobs=HOST_CPUS
build:ci --jobs=HOST_CPUS
build:ci --verbose_failures
build:ci --repository_cache=~/.cache/bazel-repo
```

### Issue B6: Add test sharding to top 5 unsharded test suites

| Field | Value |
|-------|-------|
| **Impact** | 2–3× test speedup for largest suites |
| **Effort** | 30 min (one-line change per target) |

Only `resvg_test_suite_impl` has `shard_count = 16`. All other tests run single-threaded.

| Target | Test Cases | Recommended Shards |
|--------|-----------|-------------------|
| `base_tests` | ~445 | 10 |
| `svg_tests` | ~417 | 10 |
| `parser_tests` | ~176 | 5 |
| `core_tests` | ~140 | 4 |
| `css_tests` | ~104 | 3 |

GTest supports sharding natively via `GTEST_SHARD_INDEX`/`GTEST_TOTAL_SHARDS` which Bazel sets
automatically.

---

## P2 — Medium Impact

### Issue B7: Replace 3×2 variant matrix with 4 product-tier CI matrix

| Field | Value |
|-------|-------|
| **Impact** | Meaningful coverage per variant + text flag bug fix |
| **Effort** | 3–4 hours |

**Bug found:** The `_multi_transition` in `rules.bzl:201` sets `text_full` but **never sets
`//donner/svg/renderer:text`**. Since `text` defaults to `false`, all 6 transitioned variants run
without text. The `*_text` and `*_text_full` suffixes produce identical binaries. The "6 variants"
are really 3 (one per backend, all without text).

**Also:** The transitioned variants don't propagate `shard_count`, so each runs as a single
enormous test process instead of 16 shards.

**Proposed fix:** Replace the 3×2 combinatorial matrix with 4 named product tiers that map to
actual Donner build configurations:

| Tier | Backend | Filters | Text | Bazel config | CI trigger |
|------|---------|---------|------|-------------|------------|
| **donner-tiny** | tiny-skia | no | no | `--config=tiny` | All PRs |
| **donner** (default) | tiny-skia | yes | simple (stb) | `--config=text` | All PRs |
| **donner-max** | tiny-skia | yes | full (FT+HB+WOFF2) | `--config=text-full` | All PRs |
| **skia-ref** | skia | yes | full | `--config=skia --config=text-full` | main only |

**Changes needed:**
1. Fix `rules.bzl` transition to also set `//donner/svg/renderer:text=true`.
2. Add `--config=tiny` to `.bazelrc`: `common:tiny --//donner/svg/renderer:filters=false`.
3. Replace the `variants` list in the resvg test BUILD with the 4-tier matrix.
4. Add `shard_count` propagation to `donner_multi_transitioned_test`.
5. Gate `skia-ref` variant to main-only (e.g. via a `ci_full_matrix` flag or separate target).

**Key files:** `build_defs/rules.bzl`, `donner/svg/renderer/tests/BUILD.bazel`,
`.bazelrc`, `.github/workflows/main.yml`

### Issue B8: Move coverage to nightly/merge-only schedule

| Field | Value |
|-------|-------|
| **Impact** | Eliminates ~30 min job from every PR |
| **Effort** | 10 min |

Coverage runs on every push to every branch. Uses `--nocache_test_results`, full LLVM
instrumentation (2× overhead), and requires JDK 11. Change trigger to `push: branches: [main]` +
daily cron.

### Issue B9: Enable ThinLTO + gc-sections for release builds

| Field | Value |
|-------|-------|
| **Impact** | −10–20% binary size |
| **Effort** | 15 min |

No LTO configured anywhere. No `-ffunction-sections -fdata-sections` + `-Wl,--gc-sections` for
Donner's own code (only for Skia's internal build).

Add to `.bazelrc`:
```
build:binary-size --copt=-flto=thin --linkopt=-flto=thin
build:binary-size --copt=-ffunction-sections --copt=-fdata-sections
build:binary-size --linkopt=-Wl,--gc-sections
```

### Issue B10: Create forward-declaration headers for heavy types

| Field | Value |
|-------|-------|
| **Impact** | −10–20% incremental compile time |
| **Effort** | 3–5 days |

Donner has zero `*_fwd.h` files. Every header that references `Entity`, `Registry`, or component
types pulls the full definition chain. ~20 of 48 ECS includers only pass handles by
value/reference and could use a lightweight forward declaration.

### Issue B11: Split monolithic `svg_core` into focused libraries

| Field | Value |
|-------|-------|
| **Impact** | ~30% faster incremental builds |
| **Effort** | 1–2 weeks |

`svg_core` has 60 `.cc` files and 60 headers — shapes, filters, text, gradients, resources all in
one target. Every consumer recompiles and links everything.

**Proposed split:**

| Target | Contents (~files) | Rationale |
|--------|-------------------|-----------|
| `svg_core_elements` | Foundation types (~10) | Everything needs these |
| `svg_core_shapes` | Shape elements (~16) | Self-contained |
| `svg_core_filters` | FE* elements (~27) | Behind feature flag |
| `svg_core_resources` | Paint/resource elements (~15) | Paint servers |

### Issue B12: Unify coverage workflow cache key

| Field | Value |
|-------|-------|
| **Impact** | Avoid redundant coverage rebuilds |
| **Effort** | 5 min |

Coverage uses `bazelcov-` prefix and hashes non-existent `WORKSPACE*` files. Never hits the main
build cache.

### Issue B13: Optimize Bazel cache key strategy

| Field | Value |
|-------|-------|
| **Impact** | Higher cache hit rate |
| **Effort** | 15 min |

Current key includes `.bazelrc` — any comment change invalidates the entire cache. Split: use
`MODULE.bazel` + `.bazelversion` for primary key, `.bazelrc` only in restore-key prefix.

### Issue B14: Split monolithic renderer files

| Field | Value |
|-------|-------|
| **Impact** | Better incremental compile times |
| **Effort** | 3–5 days |

The four largest `.cc` files are all renderers: `RendererSkia.cc` (4,245 lines),
`RendererTinySkia.cc` (2,254), `RendererDriver.cc` (2,072), `RendererGeode.cc` (1,896).

Split by concern: `RendererSkia_Gradient.cc`, `RendererSkia_Filter.cc`, `RendererSkia_Text.cc`,
`RendererSkia_Shape.cc`. Changes to gradient code no longer recompile text code.

---

## Build Improvement Impact Summary

| Priority | Issues | CI Time Savings | Binary Size |
|----------|--------|-----------------|-------------|
| **P0** | B1 | −60–120 min/run | — |
| **P1** | B2–B6 | −30–50% build+test | — |
| **P2** | B7–B14 | −30 min/PR + incremental | −10–20% |

### Quick wins (< 1 hour total, immediate ROI):
1. B3: Increase `--local_test_jobs` (5 min)
2. B6: Add test sharding (30 min)
3. B9: Enable LTO + gc-sections (15 min)
4. B12: Fix coverage cache key (5 min)

---

# Appendix: Spec Quick Reference

| Area | Spec | Key Sections |
|------|------|-------------|
| Filter functions | [Filter Effects 1](https://www.w3.org/TR/filter-effects-1/) | §8 (functions), §15 (feMorphology) |
| Transform origin | [CSS Transforms 2](https://www.w3.org/TR/css-transforms-2/) | §6 |
| Image sizing | [SVG2](https://www.w3.org/TR/SVG2/embedded.html) + [CSS Images 3](https://www.w3.org/TR/css-images-3/) | SVG2 §5.8, CSS §5.2 |
| Text baselines | [CSS Inline 3](https://www.w3.org/TR/css-inline-3/) | §4.2, §4.4 |
| Paint order | [SVG2](https://www.w3.org/TR/SVG2/painting.html) | §13.11 |
| Context paint | [SVG2](https://www.w3.org/TR/SVG2/painting.html) | §13.3 |
| Switch | [SVG2](https://www.w3.org/TR/SVG2/struct.html) | §5.9 |
| Masking | [CSS Masking 1](https://www.w3.org/TR/css-masking-1/) | §6.1, §7.2 |
| BiDi | [CSS Writing Modes 4](https://www.w3.org/TR/css-writing-modes-4/) + [UAX #9](https://unicode.org/reports/tr9/) | §2 |
| Text on path | [SVG2](https://www.w3.org/TR/SVG2/text.html) | §11.7 |

# Appendix: Bot Consultation Notes

This doc was synthesized from analysis by:
- **SpecBot** — SVG2 spec section references, browser behavior cross-checks, priority ordering by
  test unblock count.
- **PerfBot** — Measured CI run times, identified macOS queue bottleneck, EnTT template blast
  radius, compilation/binary size analysis.
- **BazelBot** — Build graph analysis, `svg_core` split proposal, test sharding audit, cache key
  strategy, LTO/gc-sections recommendations.
- **Explore agents** — Raw data extraction from resvg test suite, renderer gaps, CI workflows.

Each bot independently identified the same top priorities (CSS filter functions, `transform-origin`,
EnTT modular headers, test sharding), giving high confidence in the recommendations.
