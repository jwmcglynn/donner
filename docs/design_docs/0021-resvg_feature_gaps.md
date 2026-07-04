# resvg-test-suite: Feature Gaps & Open Bugs

**Status:** Living catalog. The CPU-backend (RendererTinySkia) feature gaps and bugs
are the active front, and lead this backlog. Geode runs the same resvg `Params`
and thresholds as the CPU variants; backend-specific override tables are not part
of the suite policy.

The triage backlog for [0022](0022-resvg_test_suite_upgrade.md)'s Milestone 2 —
working through the tests the suite upgrade pulled in and either fixing the
underlying gap or recording why a `Params::Skip(...)` is the correct state. Each
entry corresponds to one or more skips/threshold-overrides in
[`resvg_test_suite.cc`](../../donner/svg/renderer/tests/resvg_test_suite.cc).

When a gap is fixed, delete its entry here and un-skip the tests **in the same
PR**. Golden overrides (where Donner is right and resvg's golden is wrong) live in
[0009](0009-resvg_test_suite_bugs.md), not here.

**Conventions:**
- **Impact** = number of currently-skipped (or fat-thresholded) tests the entry covers.
- **Root cause** = best-known localized explanation, or "needs investigation".
- **Next step** = what a fix PR touches first.
- Prefix `B` = bug (Donner is wrong), `F` = feature gap (standard feature not
  implemented). Numbers are stable IDs other docs/PRs can reference; retired
  entries leave their number burned.

## How a test can be "not passing"

There are **four** supported ways the suite records a known gap. All of them must
be expressed through the normal `Params` path close to the affected tests:

| State | Count | Meaning |
|---|---:|---|
| `Params::Skip("reason")` | 188 | Not run. Feature gap or known bug. The bulk of this doc. |
| `Params::RenderOnly("reason")` | 52 | Rendered, **not** compared. Used for UB/deprecated cases where no-crash coverage is still useful. |
| Commented-out `INSTANTIATE_TEST_SUITE_P` | 1 block | `filters/filter-functions` — whole category dark on CI. See [B2](#b2-filtersfilter-functions-category-disabled-on-ci). |
| `Params::WithThreshold(…, maxPx)` / local max-pixel budget | 96 | Passes with an explicit threshold or pixel budget. Large non-text budgets remain suspect; see [Masked bugs behind inflated CPU thresholds](#masked-bugs-behind-inflated-cpu-thresholds). |
| Geode-disabled local `Params` entries | 1 analytic-residual + ~9 CPU-only-feature | The analytic-coverage work closed the former ~16-gate cluster; remaining = 1 `feGaussianBlur/complex-transform` ([#625](https://github.com/jwmcglynn/donner/issues/625)) + paint-order/0-N-dash tests Geode doesn't implement yet. See [Geode coverage (resolved)](#geode-coverage-analytic-slug-dual-ray-resolved--the-misdiagnosis-correction). |

## Current totals

| | Count |
|---|---:|
| `Params::Skip(...)` | 181 (`grep -c 'Params::Skip'`, 2026-07-03) |
| `Params::RenderOnly(...)` | 52 (render-must-not-crash, no pixel compare) |
| `WithThreshold` / max-pixel overrides | 89 (`grep -cE 'WithThreshold\|WithMaxPixels'`, 2026-07-03; ~15 still over 1000 px -> masked-bug candidates) |
| Geode-disabled local `Params` entries | 1 analytic-residual + ~9 CPU-only-feature (down from 22; analytic dual-ray landed, see 0041) |
| Commented-out category blocks | 1 (`filters/filter-functions`) |

---

## Recently fixed (PRs #608–#611)

Landed 2026-05-25 from a parallel CPU-backend debugging sweep. IDs are burned (not reused).

- **F2 — `transform-origin` regression (#514)** → [#609](https://github.com/jwmcglynn/donner/pull/609).
  The pivot sandwich was written `Translate(O) * raw * Translate(-O)`, but
  `Transform2d::operator*` is left-first, so the pivot-out translate applied *last* —
  the pivot wasn't a fixed point. Swapped to `Translate(-O) * raw * Translate(O)`.
  13 tests un-skipped (9.7k–151k px → pass). The 7 paint-server/`<image>`/text cases
  were a *separate* never-implemented gap → re-filed as **F12** below.
- **B1 — intrinsic sizing + percent on non-square viewBox** → [#611](https://github.com/jwmcglynn/donner/pull/611).
  Three coupled causes: `calculateRawDocumentSize` used `transformPosition` (folded
  the letterbox translation into the size); percent resolution used the viewBox
  *diagonal* extent instead of per-axis X/Y; `<marker>` length attrs were parsed with
  a no-suffix parser that rejected `%`. 10 tests un-skipped.
- **B5 — `feMorphology` degenerate radius** → [#608](https://github.com/jwmcglynn/donner/pull/608).
  Negative/zero/empty/absent radius blanked the shape to transparent black; per
  Filter Effects §15.4 a disabled morphology passes the input through. 5 tests un-skipped.
- **B6 — `feImage` resampling** → [#610](https://github.com/jwmcglynn/donner/pull/610).
  The suspected fragment-ref-transform bug was a red herring — those 3 tests were
  never broken (their 22k–34k px thresholds were pure over-inflation, now removed).
  The real bug: tiny-skia upscaled feImage with **bilinear**; resvg uses
  **Mitchell-Netravali bicubic**. 4 subregion tests 2.6k–8.7k px → 0. `svg.svg`'s
  custom golden refreshed to bicubic. **Geode now has the matching WGSL bicubic
  sampler** (`filter_image.wgsl`, edge-clamped, RGB≤A) plus a per-attribute
  placement-rect fix in `GeodeFilterEngine::applyImage` (each of x/y/width/height
  resolved independently, percent/OBB-aware, defaulting to the filter region):
  6 of 7 Geode feImage gaps closed (embedded-png, preserveAspectRatio=none,
  with-subregion-1..4). Only `svg.svg` remains Geode-gated — its residual is the
  shared slug_fill coverage gap below, not a feImage issue.

## Priority 0: CPU-backend backlog (the active front)

Highest-value first. "Out of scope" rows are correct-as-skipped and listed at the
bottom for completeness.

> **Recently fixed (PRs #608–#611, in review) — see [Recently fixed](#recently-fixed-prs-608611).**
> F2 (transform-origin regression), B1 (intrinsic sizing), B5 (feMorphology), B6
> (feImage resampling) are resolved; their IDs are burned. The rows below are
> what's left.

| ID | Gap | Impact | Kind |
|---|---|---:|---|
| B2 | `filters/filter-functions` disabled (CI "Data corrupted") | ~30 | CI gap — whole category dark |
| B3 | `structure/image` golden kernel-era mismatch | 13 | Golden refresh + `<image>` upscale-kernel decision (see [B3](#b3-structureimage-golden-kernel-era-mismatch)) |
| F12 | `transform-origin` on `<textPath>` baseline | 1 | gradient/pattern/`<image>`/text resolve the pivot; `on-text-path` baseline still drops it → [#624](https://github.com/jwmcglynn/donner/issues/624) |
| F3 | `context-fill` / `context-stroke` | 13 | Feature |
| F7 | `paint-order` rendering | **DONE** (7/8) | Rendered on shapes + text; `on-tspan` residual → [#624](https://github.com/jwmcglynn/donner/issues/624) |
| F9 | `textLength` + `lengthAdjust` stretch/compress | 8 | Feature |
| F10 | `textPath` SVG2 attributes (`path`/`side`/`method`/`spacing`) | 8 | Feature |
| F11 | BiDi / RTL text shaping | ~8 | Feature (needs `text-full`) |
| B7 | font substitution — missing bundled families (masked by fat thresholds) | ~9 | Triage: bundle fonts vs. document as known gap |
| — | masking edge cases (mask 8, clipPath 6) | ~14 | Mixed |
| — | uncertain `Bug?` entries (need triage) | ~12 | Needs investigation |
| F1 | `enable-background` + `in=Background*` | 23 | **Out of scope** (deprecated) |
| — | other deprecated/UB skips | ~30 | **Out of scope** |

---

## Tracked regressions & disabled blocks

### B2: `filters/filter-functions` category disabled on CI

**Impact:** ~30 tests — the entire `filters/filter-functions/` block, commented out
at [`resvg_test_suite.cc:1410`](../../donner/svg/renderer/tests/resvg_test_suite.cc).

**Symptom:** The `INSTANTIATE_TEST_SUITE_P(FiltersFilterFunctions, …)` block is
commented out. The category produces `"Data corrupted"` parse errors on CI x86_64
runners but passes locally on aarch64. (Note: the harmless per-test `"Data
corrupted"` log lines from `UrlLoader` font fallback are *unrelated* — this is a
parse failure that fails the comparison.)

**Root cause:** unknown. Candidates: a resvg-test-suite data-integrity issue on
CI, an x86_64-specific parser bug, or a runfiles/encoding difference between the
runners. This is exactly the CI-vs-local gap the project's always-green-main
policy calls out — the fix is to close the gap, not route around it.

**Next step:** reproduce on an x86_64 runner (or container). Capture the exact SVG
that triggers `"Data corrupted"` and minimize it. These tests were enabled once in
[#515](https://github.com/jwmcglynn/donner/pull/515) before being disabled, so the
rendering path works — this is an input/parse problem on one arch. Two custom
goldens (`drop-shadow-function-{mm,em}-values`) are parked for re-enable; see
[0009](0009-resvg_test_suite_bugs.md).

---

## Masked bugs behind inflated CPU thresholds

These tests **pass**, but only because `maxMismatchedPixels` was raised far above
the suite default (100). pixelmatch already excludes anti-aliased pixels, so a
multi-thousand-px diff on the CPU backend is a *real* rendering difference. Per
[CLAUDE.md §"Anti-Aliasing Is Never the Root Cause"](../../CLAUDE.md), "AA drift"
is not a valid reason for these magnitudes. The full audit list lives in the test
file. **B6 (feImage resampling) is now fixed** — see [Recently fixed](#recently-fixed-prs-608611);
the real cause was a bilinear-vs-bicubic kernel, not the suspected transform bug, and
the 3 "transform" tests were never broken (their fat thresholds were over-inflation,
now removed). The remaining structural cluster is below.

### B7: font substitution — missing bundled families

**Impact:** ~9 `text/font-family/` tests at `maxPx` 600–5200 (`serif` 4200,
`sans-serif` 1900, `monospace` 600, `cursive` 5000, `fantasy` 5200,
`bold-sans-serif` 5200, `source-sans-pro` 1300, `font-list` 1300, `fallback-2`
1000), plus `text/text/xml-lang=ja` (19100, CJK) and `structure/defs/
style-inheritance-on-text` (6500).

**Symptom:** the diffs are whole-glyph — Donner substitutes a *different font* than
the golden was rendered with (the suite's `cursive`/`fantasy`/CJK families aren't
bundled), so every glyph outline differs. This is not a renderer bug; it's a
font-availability gap currently *silently* absorbed by a fat threshold.

**Next step (triage decision):** either (a) bundle the missing families and tighten
the thresholds to default, or (b) reclassify these as explicit `Skip("font not
bundled: <family>")` so the gap is visible instead of hidden. Do **not** leave them
as unexplained fat thresholds. Decide per-family; `serif`/`sans-serif`/`monospace`
likely map to already-bundled Noto faces (real diff to chase), while
`cursive`/`fantasy` are genuinely missing.

> The remaining sub-1000-px CPU thresholds (feColorMatrix matrix/saturate variants,
> feDropShadow, text-decoration rotate-lists, pattern AA) are small enough to be
> plausible coverage-geometry differences; audit opportunistically but they are not
> promoted bugs.

---

## High-leverage bugs (one root cause, many tests)

**B1 (intrinsic sizing + percent on non-square viewBox) is now fixed** — see
[Recently fixed](#recently-fixed-prs-608611). It was three coupled causes, not just
the suspected `transformPosition`→`transformVector` (also per-axis percent extent +
`<marker>` `%` parsing).

**B4 (`<use>` referencing inline `<svg>` elements) is now fixed.** Two coupled
causes: (1) the `<use>` width/height override + viewport machinery
(`LayoutSystem::createShadowSizedElementComponent`,
`ComputedShadowSizedElementComponent`) only accepted `<symbol>` targets, never
`<svg>`, so no instance viewport (clip) was created and the shadow content
transform dropped the referenced svg's `x`/`y` when it had no viewBox; (2) a CSS
shadow-tree bug — `ShadowedElementAdapter::parentElement()` looked up
`ElementTypeComponent` on the raw tree entity, so a shadow entity whose parent
was *also* a shadow entity appeared parentless, matched `:root`, and the UA
rule `svg:not(:root) { overflow: hidden }` never clipped nested
`<use>` → `<use>` → `<svg>` chains (descendant combinators through shadow
parents were broken generally). 5 tests un-skipped (70k–130k px → pass).

### B3: `structure/image` golden kernel-era mismatch

**Re-triaged (2026-07-03):** the old "embedded data URLs render at wrong size"
description was wrong. Donner's `<image>` placement, intrinsic sizing
(no-width/no-height/auto), MIME sniffing, GIF decode, and `preserveAspectRatio`
alignment are all correct (verified per-test against the goldens: alignment
diffs are zero-displacement, residuals hug resampled edges only). The real gap:
the vendored 2023 resvg-test-suite goldens were generated across several resvg
eras with different `<image>` upscale kernels:

- **Bilinear-era goldens** (external-jpeg/png, embedded-png,
  embedded-jpeg-as-*, slices, with-transform, odd-numbers, ...): match
  tiny-skia `Bilinear` — these pass today (the nine former 300-px
  "slice/on-svg" thresholds now pass at the default 100 and were removed).
- **Mitchell-era goldens** (`no-height-non-square`,
  `width-and-height-set-to-auto`): match current resvg, which upscales
  `<image>` with Mitchell-bicubic (`tiny_skia::FilterQuality::Bicubic`) —
  verified: both pass when Donner's `drawImage` is switched to `Bicubic`, and
  current upstream resvg goldens reproduce with a Mitchell kernel to mean
  |Δ| ≈ 0.2.
- **Intermediate-era goldens** (no-width/no-height/no-width-and-height,
  embedded-gif, external-gif, embedded-jpeg-without-mime,
  preserveAspectRatio=none + the three `*-meet` variants): match **neither**
  kernel (12.5k px vs Bilinear, 21.8k px vs Bicubic for the no-* group; the
  deterministic-decode GIF golden rules out decoder differences).

Plus one policy case: `embedded-svg-with-text` — resvg parses `<image>`-embedded
SVGs with an empty fontdb, so its golden renders no text; Donner renders the
text (browser-consistent).

**Next step (needs a maintainer decision):** refresh the whole
`structure/image` golden set from current resvg and adopt Mitchell-bicubic in
`RendererTinySkia::drawImage`/`drawBitmap` (+ the Geode sampler) to match
current resvg, or keep bilinear and leave the 13 mismatched-era tests skipped.
Per-test threshold inflation is not an option.

**B5 (feMorphology degenerate radius) is now fixed** — see
[Recently fixed](#recently-fixed-prs-608611).

---

## Unimplemented features (clean single-feature scope)

### F3: `context-fill` / `context-stroke`

**Impact:** 13 tests in `painting/context/`. Parsed but not honored at render.
Used by markers and `<use>` to inherit the referencing element's paint.

### F7: `paint-order` rendering

**Impact:** 8 tests in `painting/paint-order/`. The property name parses but
render order (fill/stroke/markers) is not reordered. On shapes, text, and tspan.

### F9: `textLength` + `lengthAdjust`

**Impact:** ~6 (`text/textLength` 2 + `text/lengthAdjust` 3 + `text/text-decoration`
interaction). Text stretching/compressing to a target length (`spacing` and
`spacingAndGlyphs`). The `arabic`/`arabic-with-lengthAdjust` cases pass on
text-full builds and are enabled with `.onlyTextFull()`.

### F10: `textPath` SVG2 attributes

**Impact:** 8 in `text/textPath/`: `path` attribute, `side=right`, `method=stretch`,
`spacing=auto`, `path`+`xlink:href` combinations, `filter` on textPath, plus the
deferred vertical/`writing-mode=tb` cases.

### F11: BiDi / RTL text shaping

**Impact:** ~8 across `text/direction` (2), `text/unicode-bidi` (1),
`text/text/bidi-reordering`, `text/tspan/bidi-reordering`,
`text/letter-spacing/mixed-scripts`, `text/textLength` Arabic. Needs the BiDi
algorithm + RTL shaping (`text-full`). Group as one workstream.

### F12: `transform-origin` on paint-servers / `<image>` / text

**Impact:** 1 remaining test in `structure/transform-origin/` (`on-text-path`); the
`on-gradient` ×2, `on-pattern` ×2, `on-image`, and `on-text` cases pass.

Gradient/pattern paint-servers and `<image>`/text apply the `transform-origin` pivot
as `Translate(-origin)·M·Translate(origin)` (matching the shape path; Donner's
`operator*` is left-first). For paint-servers the pivot is recomputed in the renderers
from each entity's `ComputedLocalTransformComponent` —
`RendererTinySkia::resolveGradientTransform`,
`RendererGeode::resolveGradientTransform`, and the shared pattern transform in
`RendererDriver` — not via the `getRawEntityFromParentTransform` accessor, which is
unrelated. For `<image>`/text the layout composes the resolved origin with the
content-placement transform.

`on-text-path` still renders the baseline path without the pivot, so a rotated
`<textPath>` samples its glyphs off-screen → [#624](https://github.com/jwmcglynn/donner/issues/624).

### Smaller feature gaps

| Category | Tests | Gap |
|---|---:|---|
| structure/svg | 2 | nested-svg `overflow` |
| structure/style | 1 | CSS `@import` / external CSS |
| structure/symbol | 1 | `transform` on `<symbol>` (SVG2) |
| painting/image-rendering | 2 | `image-rendering` (pixelated/crisp-edges) |
| masking/clipPath | 6 | clipPath with `<text>` children, `<use>` child, shorthand edge cases |
| masking/mask | 8 | `mask-type`, `mask-units`, `color-interpolation`, mask-on-self |
| text/font | 2 | `font` shorthand; canvas-size mismatch (test harness) |
| text/tspan | 3 | tspan interaction with `clip-path`/`filter`/`mask` |
| painting/stroke-dasharray | 4 | `0 n` dash patterns with caps; `40 0` closed-rect dash-seam (see note) |
| painting/marker | 3 | multiple closepaths, recursive-5 (rounded-rect corner fixed, [#623](https://github.com/jwmcglynn/donner/issues/623)) |

**`painting/stroke-dasharray/n-0` (`40 0`)** — root-caused under [#623](https://github.com/jwmcglynn/donner/issues/623)
and intentionally left skipped: an SVG `<rect>` is a *closed* contour, so tiny-skia
(the faithful Rust-tiny-skia port) seam-joins the first and last `40`-unit dash across
the start vertex into one continuous dash, making the start corner an interior MITER.
resvg's golden butt-caps that corner because usvg flattens the rect to a *non-closed*
path before dashing. Donner's mitered closed-contour seam is the spec-conformant
behavior (matches Skia/Chrome/Firefox); the diff is a resvg-pipeline difference, not a
Donner/tiny-skia bug. Pinned by `RendererTests.DashSeamClosedContourMitersStartCorner`.

**`painting/marker/marker-on-rounded-rect`** — fixed under [#623](https://github.com/jwmcglynn/donner/issues/623):
`Path::vertices()` now emits the arrival marker-mid at a rounded rect's zero-length-close
start corner (stacking start + mid + end, matching resvg), while still excluding smooth
all-curve loops (circle/ellipse).
| text/writing-mode | ~7 | `writing-mode=tb` with `dx`/`dy`, vertical-lr/rl edge cases, mixed-script (upright CJK + rotated Latin) column geometry (also skips `text/alignment-baseline/hanging-on-vertical`) |

---

## Needs triage (uncertain `Bug?` entries)

These have a question-mark reason in the file and need a root-cause pass to decide
bug vs. out-of-scope:

- `structure/svg`: non-UTF-8 encoding, rect-inside-non-SVG-element, xmlns validation
  (XML entity references ×3 and mixed-namespaces now pass and are enabled)
- `paint-servers/stop`: `stop-color` inherit edge case
- `text/letter-spacing/non-ASCII-character`: different CJK glyph (wrong font? →
  overlaps [B7](#b7-font-substitution--missing-bundled-families))
- `text/font-family/fallback-1`: fallback from invalid family
- `masking/clip/simple-case`: CSS2 `clip` property (`rect()` clipping on viewport
  elements, deprecated) — not implemented, 76k px diff
- `filters/feImage/empty.svg`: `std::bad_alloc` **crash** on Linux CI runners
  (passes on macOS; enabled briefly on 2026-07-03 and reverted after the Linux
  lane crashed in all variants). Likely shares a resource-loading root cause
  with [#576](https://github.com/jwmcglynn/donner/issues/576) — a failed/corrupt
  load yielding garbage dimensions would explain the giant allocation. Crash =
  "never crash on untrusted input" violation; root-cause on a Linux x86_64 env.

---

## Out of scope (correctly skipped — do not "fix")

| Category | Tests | Why |
|---|---:|---|
| filters/enable-background | 21 | Category default `Params::RenderOnly(...)`: deprecated in SVG 2 (→ `<filter>` chains / `backdrop-filter`). See [`unsupported_svg1_features.md`](../unsupported_svg1_features.md). |
| filters/filter `in=Background*` | 2 | Same deprecation (BackgroundImage/BackgroundAlpha inputs). |
| text/tref | 9 (+1 display) | `<tref>` removed in SVG 2. |
| text/kerning | 2 | `kerning` attribute deprecated SVG 1.1. |
| text/glyph-orientation-* | 2 | deprecated SVG 1.1. |
| paint-servers/radialGradient | 1 | test-suite bug (`fr>` default — SVG2 behavior changed). `focal-point-correction` now passes and is enabled. |
| structure/style-attribute | 1 | `<svg version="1.1">` disables geometry-in-style (SVG 1.1 behavior). |
| Other RenderOnly UB cases | 51 | Implementation-defined output; we verify no-crash only (per project policy, kept RenderOnly not Skip). |

---

## Geode / Resvg Override Policy

Geode is part of the same resvg test matrix as the CPU variants. It should use the
same `ImageComparisonParams` thresholds, render-only state, skips, and golden
overrides as the other renderers. Backend support is recorded through normal
`Params` feature requirements or local backend disables, never through side-table
gates.

Policy:

- Do not add `geodeCategoryGate`, `geodeFilenameGate`, or backend-specific threshold
  side tables.
- Do not maintain symptom-ledger sets such as `kEdgeFloor` or `kGenuineG2` in the
  resvg file. If a parity-only exception is truly needed, express it through the
  local `Params` override for that test, using `disableGeodeParity(...)` with a
  short reason.
- Category-wide defaults are acceptable only when every file in the category has
  the same reason. `filters/enable-background` is the model: one category default
  `Params::RenderOnly(...)`, not a per-file list. Category feature requirements
  such as `text/*` requiring text support are additive, so per-test overrides do
  not accidentally opt out of backend capability checks.
- Non-resvg regression tests belong in focused renderer test files, not in
  `resvg_test_suite.cc`. Use the resvg suite file only for resvg-test-suite data.
- Test comments should state the current expected behavior and why an override
  exists. Avoid PR history, audit logs, and long failure narratives in the test
  file; put durable analysis here instead.

The practical goal is fewer overrides over time. A large override map is a signal
to either fix the feature, classify it as a clear unsupported/deprecated case, or
write a focused non-resvg regression that exercises the root cause directly.

### Geode coverage: analytic Slug dual-ray (resolved) + the misdiagnosis correction

**RESOLVED.** Geode now uses official Slug analytic dual-ray coverage at 1 sample/pixel
on every adapter (4× MSAA and the Intel-Arc alpha-coverage fallback deleted; Mac/Linux
unified; `GeodeTinyParity` retired). See [0041](0041-geode_analytical_aa.md) (as-built).

The earlier theory in this section — that ~16 Geode gates shared one "slug_fill
edge-coverage quantization" root cause — was **wrong**, and is preserved here only as a
caution: the analytic rewrite left those tests **byte-identical**, *proving* coverage was
never the cause. They were three real, separate bugs plus two legitimate per-backend
goldens, all now fixed/closed:

- `filters/feConvolveMatrix/*` (10) + `filters/feMorphology/source-with-opacity` —
  a **pattern-tile filter-region-scissor leak** (`beginPatternTile` didn't clear the
  outer clip stack, shifting tiled cells ~1px) + a missing feMorphology linearRGB
  round-trip. Both fixed → 0 px.
- `structure/svg/preserveAspectRatio=xMinYMin` + `proportional-viewBox` — were
  parity-only; pass once `GeodeTinyParity` is retired.
- `painting/marker/orient=auto-on-M-L-Z` — degenerate zero-area closed stroke
  decomposed into overlapping triangles; fixed by de-closing collinear closed subpaths
  before `strokeToFill` → 0 px.
- `filters/feColorMatrix/type=matrix-with-non-normalized-values` + `filters/feImage/svg`
  — Geode verified-correct, differs from resvg's finite-sample reference; **per-backend
  Geode goldens** (`withGeodeGoldenOverride`).

**Lesson:** a large diff amplified by a filter/matrix is not evidence of a coverage
problem — inspect whether a coverage change actually moves it before attributing it.
The only remaining Geode resvg gate is `feGaussianBlur/complex-transform` (genuine
analytic-vs-finite-sample 1px blur edge) — [#625](https://github.com/jwmcglynn/donner/issues/625).

---

## Template for new entries

```markdown
### Bn or Fn: Short title

**Impact:** N tests.

**Symptom:** (What does the diff look like?)

**Root cause:** (file:line if known; "needs investigation" otherwise.)

**Next step:** (Concrete action for a fix PR.)

**Affected tests:**
- path/to/first-test.svg
```
