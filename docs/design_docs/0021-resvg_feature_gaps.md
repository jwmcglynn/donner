# resvg-test-suite: Feature Gaps & Open Bugs

**Status:** Living catalog (current as of 2026-05-25). **Geode parity closed in
[#606](https://github.com/jwmcglynn/donner/pull/606)** — the GPU backend is now at
content parity with the CPU rasterizer (residual items are the accepted 4× MSAA
edge floor + a short G3/G4/G5 tail). **The CPU-backend (RendererTinySkia) feature
gaps and bugs are now the active front, and lead this backlog.**

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

There are **four** ways the suite hides a gap — not just `Skip`. The fourth is the
one most likely to mask a real bug, and was under-tracked before this revision:

| State | Count | Meaning |
|---|---:|---|
| `Params::Skip("reason")` | 288 | Not run. Feature gap or known bug. The bulk of this doc. |
| `Params::RenderOnly("UB: …")` | 51 | Rendered, **not** compared. Implementation-defined / UB input; we only assert no-crash. Correctly classified — [out of scope](#out-of-scope-correctly-skipped--do-not-fix). |
| Commented-out `INSTANTIATE_TEST_SUITE_P` | 1 block | `filters/filter-functions` — whole category dark on CI. See [B2](#b2-filtersfilter-functions-category-disabled-on-ci). |
| `Params::WithThreshold(…, maxPx)` with **large `maxPx`** | ~22 over 1000 px | **Passes, but only because the diff cap is inflated.** Per [CLAUDE.md](../../CLAUDE.md) (>1000 px non-text = broken feature), several of these are **masked bugs**, not "AA". New section: [Masked bugs behind inflated CPU thresholds](#masked-bugs-behind-inflated-cpu-thresholds). |

Geode's separate binary parity gate (`geodeParityGate`) is a fifth axis, now
nearly closed — see [Geode parity — resolved](#geode-parity--resolved-606).

## Current totals

| | Count |
|---|---:|
| `Params::Skip(...)` | 288 |
| `Params::RenderOnly(...)` | 51 (UB — render-must-not-crash, no pixel compare) |
| CPU `WithThreshold` overrides (tiny-skia) | 87 (of which ~22 exceed 1000 px → masked-bug candidates) |
| Commented-out category blocks | 1 (`filters/filter-functions`) |
| Geode binary parity gate (`geodeParityGate`) | 173 (172 edge-floor + 1 `feColorMatrix/hueRotate`); 0 text, 0 other genuine |

---

## Priority 0: CPU-backend backlog (the active front)

Highest-value first. "Out of scope" rows are correct-as-skipped and listed at the
bottom for completeness.

| ID | Gap | Impact | Kind |
|---|---|---:|---|
| **F2** | `transform-origin` rendering **regression** (#514) | 20 | **Regression — single PR, known-good prior state. Top payoff.** |
| **B6** | `feImage` fragment-ref + transform broken (masked by fat thresholds) | 7 | **Bug — 22k–34k px diffs hidden at `maxPx`; not AA.** |
| B2 | `filters/filter-functions` disabled (CI "Data corrupted") | ~30 | CI gap — whole category dark |
| B3 | `<image>` embedded/data-URL sizing | 18 | Bug — one investigation |
| B1 | intrinsic sizing + percent on non-square viewBox | ~10 | Bug — one root cause, many categories |
| B5 | `feMorphology` edge cases | 5 | Bug |
| B4 | `<use>` → inline `<svg>` sizing | 5 | Bug (shares machinery with B3) |
| F3 | `context-fill` / `context-stroke` | 13 | Feature |
| F5 | full `dominant-baseline` keyword set | 14 | Feature |
| F4 | `<switch>` conditional processing | 12 (+systemLanguage 3) | Feature |
| F6 | full `alignment-baseline` keyword set | 10 | Feature |
| F7 | `paint-order` rendering | 8 | Feature (parsed, not rendered) |
| F9 | `textLength` + `lengthAdjust` stretch/compress | 8 | Feature |
| F10 | `textPath` SVG2 attributes (`path`/`side`/`method`/`spacing`) | 8 | Feature |
| F11 | BiDi / RTL text shaping | ~8 | Feature (needs `text-full`) |
| F8 | primitive subregion clipping (feBlend/feComposite/feFlood) | 5 | Feature |
| B7 | font substitution — missing bundled families (masked by fat thresholds) | ~9 | Triage: bundle fonts vs. document as known gap |
| — | masking edge cases (mask 8, clipPath 6) | ~14 | Mixed |
| — | uncertain `Bug?` entries (need triage) | ~12 | Needs investigation |
| F1 | `enable-background` + `in=Background*` | 23 | **Out of scope** (deprecated) |
| — | other deprecated/UB skips | ~30 | **Out of scope** |

---

## Tracked regressions & disabled blocks

### F2: `transform-origin` rendering broken after #514

**Impact:** 20 tests — all of `structure/transform-origin/` (category
`StructureTransformOrigin`, [`resvg_test_suite.cc:2229`](../../donner/svg/renderer/tests/resvg_test_suite.cc)).

**Symptom:** 10K–150K-pixel diffs — rendering is completely wrong, not slightly
off. Every test tagged `Skip("transform-origin broken after #514")`.

**Root cause:** PR #514 added single-keyword `transform-origin` parsing (e.g.
`transform-origin: center`) and regressed the rendering path in the process.
Before #514 the two-token forms applied correctly; after #514 the transform
pivots around the wrong origin.

**Next step:** bisect #514's diff. The parser change and the apply-at-render change
are separable — verify `ParseTransformOrigin` output, then check
`LayoutSystem::createComputedLocalTransformComponentWithStyle` consumes the
resolved origin. Land the fix and un-skip all 20 in one PR. **Highest single-PR
payoff in the backlog** (it's a regression, so there's a known-good prior state).

> Note: this entry previously described `transform-origin` as a never-implemented
> *presentation attribute*. That gap was closed; the current failure is the #514
> *regression* above. The presentation-attribute form should be re-verified once
> the regression is fixed.

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
file; the two structural clusters are promoted to tracked bugs below.

### B6: `feImage` fragment-ref + transform broken (CPU)

**Impact:** 7 tests in `filters/feImage/`, all "passing" at inflated caps:

| Test | `maxPx` | Comment in file |
|---|---:|---|
| `link-to-an-element-with-transform.svg` | 34200 | "Fragment ref with skewX transform on element" |
| `link-on-an-element-with-complex-transform.svg` | 26200 | "Fragment ref with complex transform" |
| `chained-feImage.svg` | 22000 | "Chained feImage fragment refs" |
| `with-subregion-4.svg` | 15000 | "Absolute subregion coords" |
| `with-subregion-3.svg` | 14500 | "Percentage width subregion" |
| `with-subregion-1.svg` / `with-subregion-2.svg` | 5100 | "OBB subregion bilinear / percentage" |

**Symptom:** a 34K-px diff on a single image means the **transform applied to a
`feImage` fragment reference is wrong** (placement/scale/skew), and subregion
placement disagrees with the golden. This is the same family as the Geode feImage
divergence that #606 chased (resolved there with the shared-`RendererDriver`
re-draw fix + driver-independent placement) — the CPU path was never audited, just
thresholded.

**Root cause:** needs investigation. Start at the `feImage` primitive's handling of
(a) a fragment reference to a transformed element, and (b) the
`x`/`y`/`width`/`height` subregion → device mapping. Cross-check against the Geode
feImage fixes in [0017](0017-geode_renderer.md) / the #606 filter work — the
correct placement math may already exist there.

**Next step:** drop the thresholds to the default 100, capture
`actual/expected/diff` PNGs for `link-to-an-element-with-transform`, and root-cause
the transform composition. Overlaps [B3](#b3-image-embedded--data-url-sizing)
(image placement) and [F8](#f8-primitive-subregion-clipping) (subregion).

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

### B1: Intrinsic sizing + percent resolution on non-square viewBox

**Impact:** ~10 tests across 7 categories (all carry the reason string
`"… intrinsic sizing + percent resolution with non-square viewBox; see
ShapesEllipse"`).

**Symptom:** For SVGs with a non-square `viewBox` (e.g. `0 0 200 100`) and no
explicit `width`/`height`, the intrinsic document size is wrong. Under the
fixture's `setCanvasSize(500, 500)`, Donner produces 500×375 instead of 500×250,
so percent-valued geometry (`cx="50%"`, `rx="40%"`, …) resolves against the wrong
viewport and renders oversized / off-center.

**Root cause:** `LayoutSystem::calculateRawDocumentSize`
([LayoutSystem.cc:806](../../donner/svg/components/layout/LayoutSystem.cc)) uses
`transformPosition()` on the viewBox→content transform, which folds in the
letterbox translation. `transformVector()` (no translation) is the correct call.
The percent-resolution pipeline is coupled to this, so fix both together.

**Next step:** swap `transformPosition`→`transformVector`, audit percent
resolution in `ComputedShapeComponent`/`LayoutSystem`, land as one PR.

**Affected tests:** `shapes/ellipse/percent-values{,-missing-ry}`,
`shapes/line/percent-units`, `shapes/rect/percentage-values-{1,2}`,
`paint-servers/linearGradient/gradientUnits=userSpaceOnUse-with-percent`,
`paint-servers/radialGradient/gradientUnits=objectBoundingBox-with-percent`,
`painting/marker/percent-values`,
`text/text/percent-value-on-{x-and-y,dx-and-dy}`.

### B3: `<image>` embedded / data-URL sizing

**Impact:** 18 tests in `structure/image/` (plus 2 external-URL `Not impl`, 4 UB
RenderOnly).

**Symptom:** Embedded images (data URLs, embedded PNG/JPEG/GIF/16-bit/SVG) render
but at the wrong size; `preserveAspectRatio` modes
(`none`/`xMin/Mid/Max…-meet`/`slice`) and the `no-width`/`no-height`/`auto` sizing
cases disagree with the golden.

**Root cause:** needs investigation — `<image>` layout/sizing and
`preserveAspectRatio` resolution for raster + nested-SVG content.

**Next step:** start with `embedded-png` + `preserveAspectRatio=none` (the
simplest), then walk the no-width/no-height/auto matrix. Shares
`preserveAspectRatio` math with [B6](#b6-feimage-fragment-ref--transform-broken-cpu)
and [B4](#b4-use-referencing-inline-svg-elements).

### B4: `<use>` referencing inline `<svg>` elements

**Impact:** 5 tests in `structure/use/`.

**Symptom:** `<use>` of an inline `<svg>` with various `width`/`height`/`viewBox`
combinations sizes the instance wrong.

**Next step:** likely shares machinery with B3's viewport sizing; investigate
together if convenient.

### B5: `feMorphology` edge cases

**Impact:** 5 tests in `filters/feMorphology/` (`empty-radius`, `negative-radius`,
`no-radius`, `radius-with-too-many-values`, `zero-radius`).

**Symptom:** degenerate/invalid radius values aren't handled to spec.

**Next step:** add radius validation/clamping in the feMorphology primitive per
Filter Effects §15.

---

## Unimplemented features (clean single-feature scope)

### F3: `context-fill` / `context-stroke`

**Impact:** 13 tests in `painting/context/`. Parsed but not honored at render.
Used by markers and `<use>` to inherit the referencing element's paint.

### F4: `<switch>` conditional processing

**Impact:** 12 tests in `structure/switch/` (+1 in `clipPath`). Includes
`requiredFeatures` / `systemLanguage` evaluation. Related: `structure/systemLanguage`
(3), which F4 should subsume.

### F5: full `dominant-baseline` keyword set

**Impact:** 14 tests in `text/dominant-baseline/`. Missing `before-edge`,
`after-edge`, `no-change`, `reset-size`, `use-script`, etc.

### F6: full `alignment-baseline` keyword set

**Impact:** 10 tests in `text/alignment-baseline/`. Full keyword set + tspan
baseline alignment.

### F7: `paint-order` rendering

**Impact:** 8 tests in `painting/paint-order/`. The property name parses but
render order (fill/stroke/markers) is not reordered. On shapes, text, and tspan.

### F8: primitive subregion clipping

**Impact:** 5 tests (`filters/feBlend` 2, `filters/feComposite` 3 incl. feFlood
subregion). Filter primitives don't clip output to their `x`/`y`/`width`/`height`
subregion. Overlaps [B6](#b6-feimage-fragment-ref--transform-broken-cpu)'s subregion cases.

### F9: `textLength` + `lengthAdjust`

**Impact:** ~8 (`text/textLength` 4 + `text/lengthAdjust` 3 + `text/text-decoration`
interaction). Text stretching/compressing to a target length (`spacing` and
`spacingAndGlyphs`), including the Arabic cases.

### F10: `textPath` SVG2 attributes

**Impact:** 8 in `text/textPath/`: `path` attribute, `side=right`, `method=stretch`,
`spacing=auto`, `path`+`xlink:href` combinations, `filter` on textPath, plus the
deferred vertical/`writing-mode=tb` cases.

### F11: BiDi / RTL text shaping

**Impact:** ~8 across `text/direction` (2), `text/unicode-bidi` (1),
`text/text/bidi-reordering`, `text/tspan/bidi-reordering`,
`text/letter-spacing/mixed-scripts`, `text/textLength` Arabic. Needs the BiDi
algorithm + RTL shaping (`text-full`). Group as one workstream.

### Smaller feature gaps

| Category | Tests | Gap |
|---|---:|---|
| structure/a | 3 | `<a>` hyperlink rendering |
| structure/svg | 2 | nested-svg `overflow` |
| structure/style | 1 | CSS `@import` / external CSS |
| structure/symbol | 1 | `transform` on `<symbol>` (SVG2) |
| painting/image-rendering | 2 | `image-rendering` (pixelated/crisp-edges) |
| masking/clipPath | 6 | clipPath with `<text>` children, `<use>` child, shorthand edge cases |
| masking/mask | 8 | `mask-type`, `mask-units`, `color-interpolation`, mask-on-self |
| text/font | 2 | `font` shorthand; canvas-size mismatch (test harness) |
| text/tspan | 3 | tspan interaction with `clip-path`/`filter`/`mask` |
| painting/stroke-dasharray | 4 | `0 n` dash patterns with caps |
| painting/marker | 4 | multiple closepaths, rounded-rect corners, recursive-5 |
| text/writing-mode | ~6 | `writing-mode=tb` with `dx`/`dy`, vertical-lr/rl edge cases |

---

## Needs triage (uncertain `Bug?` entries)

These have a question-mark reason in the file and need a root-cause pass to decide
bug vs. out-of-scope:

- `structure/svg`: XML Entity references (3), mixed namespaces, non-UTF-8 encoding,
  rect-inside-non-SVG-element, xmlns validation
- `paint-servers/stop`: `stop-color` inherit edge case
- `text/letter-spacing/non-ASCII-character`: different CJK glyph (wrong font? →
  overlaps [B7](#b7-font-substitution--missing-bundled-families))
- `text/textLength/on-text-and-tspan`: we compress more than the golden
- `text/font-family/fallback-1`: fallback from invalid family
- `masking/clip/simple-case`: empty `Skip()` with no reason — must get a reason or
  be fixed
- `filters/feImage/empty.svg`: `Skip("Linux CI: std::bad_alloc in test setup")` —
  a CI-only allocation failure that should be root-caused, not left skipped

---

## Out of scope (correctly skipped — do not "fix")

| Category | Tests | Why |
|---|---:|---|
| filters/enable-background | 21 | `enable-background` deprecated in SVG 2 (→ `<filter>` chains / `backdrop-filter`). See [`unsupported_svg1_features.md`](../unsupported_svg1_features.md). |
| filters/filter `in=Background*` | 2 | Same deprecation (BackgroundImage/BackgroundAlpha inputs). |
| text/tref | 9 (+1 display) | `<tref>` removed in SVG 2. |
| text/kerning | 2 | `kerning` attribute deprecated SVG 1.1. |
| text/glyph-orientation-* | 2 | deprecated SVG 1.1. |
| paint-servers/radialGradient | 2 | test-suite bugs (`focal-point-correction`, `fr>` default — SVG2 behavior changed). |
| painting/opacity/50percent | 1 | css-color-4 allows percentage; test predates it. |
| structure/style-attribute | 1 | `<svg version="1.1">` disables geometry-in-style (SVG 1.1 behavior). |
| RenderOnly UB cases | 51 | Implementation-defined output; we verify no-crash only (per project policy, kept RenderOnly not Skip). |

---

## Geode parity — resolved (#606)

The GPU backend ([Geode](0017-geode_renderer.md)) reached **content parity** with
RendererTinySkia across the resvg suite in
[#606](https://github.com/jwmcglynn/donner/pull/606). Parity is measured
geode-vs-**tiny-skia** (not geode-vs-resvg-golden, which carries a baseline/crosshair
offset that fakes bugs), via the in-process backend matrix
([0017 §Phase 4b](0017-geode_renderer.md)): one geode binary runs `TinyGolden`,
`GeodeGolden`, and `GeodeTinyParity` modes. The parity gate is pixelmatch
`includeAA=false`, per-pixel `kDefaultThreshold` (0.02), **flat 100-px max-count, no
per-test thresholds**; tests over 100 px are binary-gated in `geodeParityGate`.

**Gate ledger (current): 173 gated = 172 edge-floor + 1 G2 + 0 text.** The
structural work is done; what remains is one driver-divergent color case and the
accepted AA floor.

- **G1 — text parity ✅ complete.** All structural text divergences resolved
  (text/decoration fill + stroke with the correct fill rule, gradient/pattern on
  text incl. span-override precedence, per-char dy/rotate, a shared-layout
  baseline-shift idempotency bug). `kGenuineText` is empty. Developer reference:
  [0038](0038-geode_tinyskia_text_parity.md).
- **G2 — filter primitives ✅ complete (1 residual).** All 37 original filter
  divergences fixed (a systematic linearRGB `color-interpolation-filters` sweep, an
  feImage shared-`RendererDriver` re-draw idempotency fix, feDisplacementMap
  correctness, feSpecularLighting spec clamp, anisotropic transformed-blur). **One
  residual:** `filters/feColorMatrix/type=hueRotate` diverges >100 px on CI's Mesa
  **llvmpipe** (≤100 px on macOS Metal) — gated as `kGenuineG2`, a driver-divergent
  color-math case.
- **G3 — driver / perf one-offs (residual).** `feMorphology/huge-radius` >30 s on
  llvmpipe (watchdog; ties into [0030](0030-geode_performance.md)); `marker
  orient=auto` tangent at curve cusps (real geometry bug). Low priority.
- **G4 — color-emoji / bitmap fonts (residual, structural).** CBDT/CBLC bitmap
  glyphs are skipped inside `drawText` ([RendererGeode.cc](../../donner/svg/renderer/RendererGeode.cc)) —
  they need the `GeodeTextureEncoder` path. Overlaps the CPU CJK gap in
  [B7](#b7-font-substitution--missing-bundled-families).
- **G5 — premultiply / color-output audit (open).** ~137 non-text tests show a
  uniform, sub-perceptual color/alpha offset across solid fills — they **pass** the
  0.02 parity gate but the whole fill region differs at strict-0. Likely one
  premultiplied-alpha / unpremult-on-store rounding root cause in Geode's solid +
  filter output path; one fix likely clears most. Repro: run `GeodeTinyParity` at
  threshold 0 and diff a solid-fill test (`painting/fill/rgb-0-127-0-0.5`). AA
  background + the accepted 4× MSAA edge floor: [0041 §2](0041-geode_analytical_aa.md).

> The 172 `kEdgeFloor` entries are the **accepted-by-design** 4× MSAA edge-coverage
> quantization (content matches tiny-skia; the sub-pixel edge differs). Geode stays
> at 4× MSAA for performance — these ratchet out together if/when finer AA lands,
> not one-by-one. Do **not** widen per-test thresholds to "fix" them.

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
