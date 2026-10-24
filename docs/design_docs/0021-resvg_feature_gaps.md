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

There are **five** supported ways the suite records a known gap. All of them must
be expressed through the normal `Params` path close to the affected tests:

| State                                                      | Count | Meaning                                                                                                                                                                                  |
| ---------------------------------------------------------- | ----: | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Params::Skip("reason")`                                   |   133 | Not run. Feature gap or known bug. The bulk of this doc.                                                                                                                                 |
| `Params::RenderOnly("reason")`                             |    58 | Rendered, **not** compared. Used for UB/deprecated cases where no-crash coverage is still useful.                                                                                        |
| Commented-out `INSTANTIATE_TEST_SUITE_P`                   |     0 | Every vendored category is instantiated.                                                                                                                                                 |
| `Params::WithThreshold(…, maxPx)` / local max-pixel budget |   103 | Passes with an explicit threshold or pixel budget. Large non-text budgets remain suspect; see [Masked bugs behind inflated CPU thresholds](#masked-bugs-behind-inflated-cpu-thresholds). |
| Geode-disabled local `Params` entries                      |     0 | Geode now runs every active resvg case. Verified analytic edge residuals use exact per-backend goldens instead of disabling the backend or inflating thresholds.                         |

## Current totals

|                                       |                                                                                                                                                      Count |
| ------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------: |
| `Params::Skip(...)`                   |                                                                                                                            133 (`grep -o 'Params::Skip('`) |
| `Params::RenderOnly(...)`             |                                                                                                               58 (render-must-not-crash, no pixel compare) |
| `WithThreshold` / max-pixel overrides | 103 call sites: 79 `WithThreshold`, 11 `WithMaxPixels`, and 13 direct `withMaxPixelsDifferent` calls. Large non-text budgets remain masked-bug candidates. |
| Geode-disabled local `Params` entries |                                                                                                                      0 (all active cases now run on Geode) |
| Commented-out category blocks         |                                                                                                                                                          0 |

---

## Recently fixed (PRs #608–#611)

Landed 2026-05-25 from a parallel CPU-backend debugging sweep. IDs are burned (not reused).

- **F2 — `transform-origin` regression (#514)** → [#609](https://github.com/jwmcglynn/donner/pull/609).
  The pivot sandwich was written `Translate(O) * raw * Translate(-O)`, but
  `Transform2d::operator*` is left-first, so the pivot-out translate applied _last_ —
  the pivot wasn't a fixed point. Swapped to `Translate(-O) * raw * Translate(O)`.
  13 tests un-skipped (9.7k–151k px → pass). The 7 paint-server/`<image>`/text cases
  were a _separate_ never-implemented gap → re-filed as **F12** below.
- **B1 — intrinsic sizing + percent on non-square viewBox** → [#611](https://github.com/jwmcglynn/donner/pull/611).
  Three coupled causes: `calculateRawDocumentSize` used `transformPosition` (folded
  the letterbox translation into the size); percent resolution used the viewBox
  _diagonal_ extent instead of per-axis X/Y; `<marker>` length attrs were parsed with
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
  All 7 Geode feImage cases are active. `svg.svg` uses an exact Geode golden for
  its verified analytic edge-coverage residual, rather than disabling the backend.

## Priority 0: CPU-backend backlog (the active front)

Highest-value first. "Out of scope" rows are correct-as-skipped and listed at the
bottom for completeness.

> **Recently fixed (PRs #608–#611, in review) — see [Recently fixed](#recently-fixed-prs-608611).**
> F2 (transform-origin regression), B1 (intrinsic sizing), B5 (feMorphology), B6
> (feImage resampling) are resolved; their IDs are burned. The rows below are
> what's left.

| ID  | Gap                                                                     |                         Impact | Kind                                                                                                                                                                                                |
| --- | ----------------------------------------------------------------------- | -----------------------------: | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| B2  | `filters/filter-functions` coverage                                     | **DONE** (26/43 files compare) | Category active; 17 mismatches are explicit triage skips.                                                                                                                                           |
| B3  | `structure/image` golden kernel-era mismatch                            |                             12 | Golden refresh + `<image>` upscale-kernel decision (see [B3](#b3-structureimage-golden-kernel-era-mismatch))                                                                                        |
| F12 | `transform-origin` on `<textPath>` baseline                             |                       **DONE** | Resolved by #868; all category cases are active.                                                                                                                                                    |
| F7  | `paint-order` rendering                                                 |               **DONE** (14/14) | Shapes and text run on both backends; `on-tspan` uses a project-owned oracle because the vendored PNG breaks cross-span kerning.                                                                    |
| F9  | `textLength` + `lengthAdjust` stretch/compress                          |                              8 | Feature                                                                                                                                                                                             |
| F10 | Remaining `textPath` vertical, kerning, and filter cases                |                              4 | Feature                                                                                                                                                                                             |
| F11 | BiDi / RTL text shaping                                                 |                             ~8 | Feature (needs `text-full`)                                                                                                                                                                         |
| B7  | font substitution — missing bundled families (masked by fat thresholds) |                             ~9 | Triage: bundle fonts vs. document as known gap                                                                                                                                                      |
| —   | masking edge cases (mask 3, clipPath 5)                                 |                              8 | `mask-type` and vector text clip children are active on both backends; remaining cases are units, transforms, color interpolation, bitmap text clips, nested intersections, and shorthand geometry. |
| —   | uncertain `Bug?` entries (need triage)                                  |                            ~12 | Needs investigation                                                                                                                                                                                 |
| F1  | `enable-background` + `in=Background*`                                  |                             23 | **Out of scope** (deprecated)                                                                                                                                                                       |
| —   | other deprecated/UB skips                                               |                            ~30 | **Out of scope**                                                                                                                                                                                    |

---

## Tracked regressions & disabled blocks

### B2: `filters/filter-functions` category coverage

**Resolved as a coverage gap.** All 43 files are instantiated. A combined TinySkia and Geode run
executes 86 comparisons: 53 passed before exception classification, representing 26 files on both
backends plus one TinySkia-only pass. Seventeen unique mismatching files now carry explicit
normal-path `Triage:` skips for blur, color-adjust, drop-shadow, or URL-chain cases. No category
remains dark, but these skips are not yet classified as Donner defects.

**Next step:** classify the 17 mismatches against the normative grammar and independent browser
pixels, then fix only confirmed Donner defects through their owning blur, color-adjust,
drop-shadow, and URL-chain paths. Remove each local skip only after that classification, a focused
regression, and both comparison modes pass; do not restore the category-level disable.

---

## CSS filter reference stages

Each URL in a CSS filter list receives the previous stage's output as its own `SourceGraphic`
and `SourceAlpha`. Named results remain local to that filter element; its filter region,
primitive units and color space remain intact. Adjacent CSS functions share an sRGB stage.
An invalid URL disables the chain, while a valid empty filter produces transparent output;
a later flood can still paint over that output. The aggregate 32-node limit and frame budgets
apply across the stages, and rejected preparation suppresses the whole filtered subtree.

Independent geometric tests cover repeated references, input and result scope, filter regions,
object-bounding-box units, empty stages, expanded captures and original CPU paint inputs. Nested
captures preserve their coordinate offsets through groups and use existing renderer surfaces;
GPU stages remain on the GPU. These contracts do not replace or widen corpus pixel tolerances. The
`invalid-FuncIRI` and `one-invalid-url-in-list` PNGs encode incompatible invalid-URL behavior;
they retain explicit render-only qualifications backed by visible-geometry assertions, documented
in [0009](0009-resvg_test_suite_bugs.md#invalid-filter-url-reference-qualifications).

## External resource policy

The resvg harness uses a test-only loader that maps the two exact HTTPS identifiers in
`structure/image/url-to-png.svg` and `url-to-svg.svg` to committed local copies. Other URL schemes
are rejected, and ordinary local references still use `SandboxedFileResourceLoader`. Snapshot
reads are bounded to 512 KiB; attribution and source hashes are recorded beside the files. Tests
perform no live network requests. Applications independently provide resource bytes through
`ResourceLoaderInterface`; deterministic image-loader tests cover accepted PNG/SVG bytes, denied
requests and resource-size limits.

The local `structure/use/xlink-to-an-external-file.svg` resource resolves and renders its text.
Its upstream reference is entirely opaque, whereas the SVG canvas is transparent outside painted
content. The comparison remains gated pending background and typography reference qualification.
An independently authored inline-versus-external text-fragment comparison covers host style
inheritance, fragment selection, the callback identifier and transparent canvas behavior.

## Miter-clip stroke joins

Both renderers distinguish `miter-clip` from the bevel fallback of ordinary `miter`. The shared
outline stroker clips the exterior join at the configured bisector distance, including exact and
near reversals, without constructing an unbounded tip. Selection, marquee, culling, range and
thumbnail bounds include the clipped-corner extent. The original `miter-clip.svg` corpus case uses
its ordinary comparison settings; analytic geometry tests and sanitizer corpus seeds cover the
bounded construction.

This is support for the existing parsed join value and the geometry described in the
[SVG Strokes draft](https://www.w3.org/TR/svg-strokes/#StrokeLinejoinProperty), not a claim that all
experimental stroke joins are implemented. `arcs` retains its miter fallback and remains a separate
reference/feature qualification case.

## Masked bugs behind inflated CPU thresholds

These tests **pass**, but only because `maxMismatchedPixels` was raised far above
the suite default (100). pixelmatch already excludes anti-aliased pixels, so a
multi-thousand-px diff on the CPU backend is a _real_ rendering difference. "AA drift"
is not a valid reason for these magnitudes. The full audit list lives in the test
file. **B6 (feImage resampling) is now fixed** — see [Recently fixed](#recently-fixed-prs-608611);
the real cause was a bilinear-vs-bicubic kernel, not the suspected transform bug, and
the 3 "transform" tests were never broken (their fat thresholds were over-inflation,
now removed). The remaining structural cluster is below.

### B7: font substitution — missing bundled families

**Impact:** ~9 `text/font-family/` tests at `maxPx` 600–5200 (`serif` 4200,
`sans-serif` 1900, `monospace` 600, `cursive` 5000, `fantasy` 5200,
`bold-sans-serif` 5200, `source-sans-pro` 1300, `font-list` 1300, `fallback-2`
1000), plus `text/text/xml-lang=ja` (19100, CJK) and `structure/defs/ style-inheritance-on-text` (6500).

**Symptom:** the diffs are whole-glyph — Donner substitutes a _different font_ than
the golden was rendered with (the suite's `cursive`/`fantasy`/CJK families aren't
bundled), so every glyph outline differs. This is not a renderer bug; it's a
font-availability gap currently _silently_ absorbed by a fat threshold.

**Next step (triage decision):** either (a) bundle the missing families and tighten
the thresholds to default, or (b) reclassify these as explicit `Skip("font not bundled: <family>")` so the gap is visible instead of hidden. Do **not** leave them
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
was _also_ a shadow entity appeared parentless, matched `:root`, and the UA
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

The `embedded-svg-with-text` policy case is independently qualified: the vendored reference omits
the embedded SVG's text, while Donner uses the document font catalog. An authored
`EmbeddedSvgUsesDocumentFontContext` case checks embedded-versus-direct identity plus visible text
ink in `//donner/svg/renderer/tests:renderer_public_api_tests` and its
`//donner/svg/renderer/tests:renderer_public_api_tests_geode` variant, so the corpus row is
render-only rather than a missing feature.

**Next step (needs a maintainer decision):** refresh the whole
`structure/image` golden set from current resvg and adopt Mitchell-bicubic in
`RendererTinySkia::drawImage`/`drawBitmap` (+ the Geode sampler) to match
current resvg, or keep bilinear and leave the 12 kernel-dependent tests skipped.
Per-test threshold inflation is not an option.

The kernel measurements above were introduced in `be5f999367`; reproduction metadata and raw
output are not included with those recorded measurements. They remain historical claims rather
than a current reproduction. Reclassification
of the 12 kernel-dependent issue #1168 rows requires a fresh run pinned to the source, decoder,
reference images, transforms, and compared kernels; the independent PNG intrinsic-placement test
separately prevents a sampling-policy decision from concealing sizing or placement regressions.

**B5 (feMorphology degenerate radius) is now fixed** — see
[Recently fixed](#recently-fixed-prs-608611).

---

## Unimplemented features (clean single-feature scope)

### F7: `paint-order` rendering

**Resolved.** All 14 tests in `painting/paint-order/` are active. Shape marker/fill/stroke
ordering and text whole-run fill/stroke ordering run on both backends. The vendored
`on-tspan` PNG loses kerning across a paint-only span boundary, so Donner uses explicit
CPU and Geode goldens that preserve continuous shaping while still gating paint order.

### F9: `textLength` + `lengthAdjust`

**Impact:** ~6 (`text/textLength` 2 + `text/lengthAdjust` 3 + `text/text-decoration`
interaction). Text stretching/compressing to a target length (`spacing` and
`spacingAndGlyphs`). The `arabic`/`arabic-with-lengthAdjust` cases pass on
text-full builds and are enabled with `.onlyTextFull()`.

Zero target length is applied to spacing in the text engine and has focused unit coverage. The
resvg `zero.svg` case remains disabled with a 397-pixel residual concentrated on the overlapping
glyph raster; its layout is no longer ignored, but the remaining oracle difference still needs
independent browser triage before the exception can be removed.

Per-span adjustment now updates the current text position inherited by following runs and stops at
the first explicit inline-axis position, including a reset later inside a span. The
`on-a-single-tspan.svg` layout is active in focused helper and DOM geometry tests; its resvg entry
remains classified for a 499-pixel small-text raster residual. Nested-container aggregation,
decoration extents, vertical layout, and textPath ordering remain implementation work.

### F10: `textPath` SVG2 attributes

Inline `path`, geometry references, `side=right`, `method=stretch`, and `spacing=auto` are
implemented. Their focused geometry and renderer contracts are active. The vendored
`method=stretch` image is identical to the align reference, so that corpus row is render-only while
independent curved-outline tests distinguish the methods. A fragment-free relative `xlink:href`
under the secure static resource policy does not resolve to a same-document element or initiate an
external fetch; its conflicting corpus image is also render-only. The
`TextPathInvalidInlineAndFragmentFreeHrefPaintsBlank` case enforces that policy in
`//donner/svg/renderer/tests:renderer_public_api_tests` and
`//donner/svg/renderer/tests:renderer_public_api_tests_geode` using a visible control scene and
a resource loader that records requests.

**Remaining impact:** 4 in `text/textPath/`: vertical/circular layout (`complex.svg` and
`writing-mode=tb.svg`), kerning across mixed children, and the known filtered-glyph coverage
residual. These belong to the vertical, shaping, and effects workstreams respectively.

### F11: BiDi / RTL text shaping

**Impact:** ~8 across `text/direction` (2), `text/unicode-bidi` (1),
`text/text/bidi-reordering`, `text/tspan/bidi-reordering`,
`text/letter-spacing/mixed-scripts`, `text/textLength` Arabic. Needs the BiDi
algorithm + RTL shaping (`text-full`). Group as one workstream.

### F12: `transform-origin` on paint-servers / `<image>` / text

**Resolved by #868.** All `structure/transform-origin/` cases, including
`on-text-path`, are active.

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

| Category                  | Tests | Gap                                                                                                                                                                                                                                                                                                                                                            |
| ------------------------- | ----: | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| structure/svg             |     0 | `structure/svg/funcIRI-with-quotes.svg` compares with ordinary budgets after the [paint URL parsing fix](../../donner/svg/properties/PropertyRegistry.cc), and `structure/svg/nested-svg-one-with-rect-and-one-with-viewBox.svg` compares with ordinary budgets after the [viewport sizing fix](../../donner/svg/components/layout/LayoutSystem.cc). |
| structure/style           |     1 | CSS `@import` / external CSS remains unsupported: imports are diagnosed and ignored, with no target requests; `UnsupportedCssImportsNeverRequestConfiguredResponses` in `//donner/svg/renderer/tests:renderer_public_api_tests` enforces that.                                                                                                                 |
| structure/symbol          |     1 | `transform` on `<symbol>` (SVG2)                                                                                                                                                                                                                                                                                                                               |
| painting/image-rendering  |     1 | Both backends implement the typed values, including two-stage non-integer `pixelated` scaling. `on-feImage` uses an independently derived nearest reference; `optimizeSpeed` remains pending reference qualification at allowed nearest-sampling ties, rather than missing SVG support.                                                                        |
| masking/clipPath          |     4 | Child clip intersections are unioned independently on both renderers. CSS circle shorthand edge cases remain. Vector text children use shared placed glyph outlines. The transformed-text vendored golden uses ink bounds, unlike SVG 2 full-glyph-cell bounds and Firefox, so it remains classified pending broader browser review.                                                                              |
| masking/mask              |     0 | Transformed group and rotated mask regions compare with ordinary budgets; `mask-type` alpha/luminance and mask `color-interpolation` sRGB/linearRGB are active.                                                                                                                                                                                                |
| text/font                 |     2 | `font` shorthand; canvas-size mismatch (test harness)                                                                                                                                                                                                                                                                                                          |
| text/tspan                |     3 | tspan interaction with `clip-path`/`filter`/`mask`                                                                                                                                                                                                                                                                                                             |
| painting/stroke-dasharray |     4 | `0 n` dash patterns with caps; `40 0` closed-rect dash-seam (see note)                                                                                                                                                                                                                                                                                         |
| painting/marker           |     3 | multiple closepaths, recursive-5 (rounded-rect corner fixed, [#623](https://github.com/jwmcglynn/donner/issues/623))                                                                                                                                                                                                                                           |

**`painting/stroke-dasharray/n-0` (`40 0`)** — root-caused under [#623](https://github.com/jwmcglynn/donner/issues/623)
and intentionally left skipped: an SVG `<rect>` is a _closed_ contour, so tiny-skia
(the faithful Rust-tiny-skia port) seam-joins the first and last `40`-unit dash across
the start vertex into one continuous dash, making the start corner an interior MITER.
resvg's golden butt-caps that corner because usvg flattens the rect to a _non-closed_
path before dashing. Donner's mitered closed-contour seam is the spec-conformant
behavior (matches Skia/Chrome/Firefox); the diff is a resvg-pipeline difference, not a
Donner/tiny-skia bug. Pinned by `RendererTests.DashSeamClosedContourMitersStartCorner`.

**`painting/marker/marker-on-rounded-rect`** — fixed under [#623](https://github.com/jwmcglynn/donner/issues/623):
`Path::vertices()` now emits the arrival marker-mid at a rounded rect's zero-length-close
start corner (stacking start + mid + end, matching resvg), while still excluding smooth
all-curve loops (circle/ellipse).
| text/writing-mode | ~7 | `vertical-lr` / `vertical-rl` edge cases and mixed-script (upright CJK plus rotated Latin) column geometry. Obsolete `tb*` SVG 1.1 values should be classified or mapped to their required compatibility aliases, not implemented as distinct modes. |

---

## Fragment image placement

An `feImage` referencing an SVG element uses the resolved primitive subregion origin for content
placement, including individually omitted `x` or `y`. Placement uses the unclipped user-space
origin; clipping the primitive or filter region does not move the referenced artwork. Normal
referenced geometry retains its natural dimensions. The CPU and Geode paths share this rule.
`filters/feImage/with-x-y.svg` and both `with-x-y-and-protruding-subregion` comparisons use ordinary
budgets. The authored renderer oracle pins `image-rendering="crisp-edges"` and compares exact
pixels for complete/partial coordinates, protrusion, and translated source content. Arbitrary host
scale, rotation, skew, and reflection are not established by this bounded coverage; their existing
exceptions remain tracked separately.

The exact placement oracle is `FeImageFragmentUsesItsOwnSubregionOrigin` in
`//donner/svg/renderer/tests:renderer_public_api_tests`; the configured GPU target is
`//donner/svg/renderer/tests:renderer_public_api_tests_geode`. The ordinary-budget corpus gates are
`//donner/svg/renderer/tests:resvg_test_suite_default_text` and
`//donner/svg/renderer/tests:resvg_test_suite_geode`; full-text uses the API with `--config=text-full`
and `//donner/svg/renderer/tests:resvg_test_suite_max`. See [Filter Effects](../filter_effects.md).

## Structure and encoding input contracts

Unknown SVG elements retain their DOM descendants but exclude the subtree from rendering,
including after incremental source insertion. The sample viewer uses a real `g` for its selection
chrome. `structure/svg/rect-inside-a-non-SVG-element.svg` and `xmlns-validation.svg` use ordinary
comparisons. The constructors and namespace-aware source projection live in `SVGUnknownElement.cc`
and `SVGDocument.cc`; `renderer_public_api_tests` covers parsed and edited wrappers, foreign
namespaces, and promotion of a known SVG container. Namespace declaration edits re-evaluate
the affected descendants while retaining their typed state for restoration; foreign shapes, text,
stylesheets, paint references, filter children, and clip geometry are excluded from their consumers.
This does not purge previously loaded or queued font faces from the existing resource cache.

The named rendering gates are `//donner/svg/renderer/tests:renderer_public_api_tests` and its
configured `renderer_public_api_tests_geode` counterpart. Corpus enforcement uses
`//donner/svg/renderer/tests:resvg_test_suite_default_text`, `resvg_test_suite_geode`, and
`resvg_test_suite_max` for the configured text lane.

The XML byte parser accepts UTF-8 input and rejects declarations naming another encoding rather
than interpreting those bytes as UTF-8. `structure/svg/not-UTF-8-encoding.svg` is an explicitly
unsupported Windows-1251 input: transcode the bytes and update or remove the declaration first.
`//donner/base/xml:xml_tests` and `//donner/svg/parser:parser_tests` cover rejection, UTF-8 BOM and
case-insensitive declaration acceptance, and unrelated encoding-like content.

## Needs triage (uncertain `Bug?` entries)

These have a question-mark reason in the file and need a root-cause pass to decide
bug vs. out-of-scope:

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

## Specification and reference classifications (#1186)

Historical `(UB)` titles are not a specification oracle. Seven assigned PNGs are byte-for-byte
copies of `third_party/resvg-test-suite/resources/ub.png`, whose SVG source draws a `UB` label.
They remain render-only corpus cases, with independent `ReferenceContract*` comparisons in
`RendererRegression_tests.cc`. No reference bitmap or comparison tolerance is changed.

| Assigned fixture | Supported behavior and reference disposition |
| --- | --- |
| `radialGradient/fr=-1` | Negative focal radius is invalid and applies no paint. UB marker reference. |
| `radialGradient/fr=0.5` | Coincident start/end circles apply no paint. UB marker reference. |
| `radialGradient/fr=0.7` | A start circle containing the end circle applies no paint; the reference instead paints a reversed gradient. Render-only, with an independent transparent-paint oracle. |
| `radialGradient/invalid-gradientTransform` | The authored all-zero matrix is singular, rather than malformed syntax. No paint; UB marker reference. |
| `radialGradient/invalid-gradientUnits` | The invalid enumeration is diagnosed and the initial `objectBoundingBox` value is retained. UB marker reference. |
| `radialGradient/negative-r` | Negative outer radius is invalid and applies no paint. UB marker reference. |
| `fill/rgb-int-int-int` | Space-separated RGB is supported CSS Color 4 syntax and matches the equivalent hex color. UB marker reference. |
| `fill/valid-FuncIRI-with-a-fallback-ICC-color` | The obsolete SVG 1.1 `icc-color()` paint grammar is unsupported. Invalid CSS declarations are rejected, preserving inherited paint; the historical reference expects a gradient. Render-only. |
| `stroke-linejoin/arcs` | The SVG Strokes draft join is not implemented; Donner retains its documented miter fallback. The reference is the UB marker, not an arcs rendering. |
| `stroke-linejoin/miter-clip` | Separate Geode/editor parity repair and ordinary reference restoration: [#2057](https://github.com/jwmcglynn/donner/pull/2057). |
| `svg/no-size` | An unsized root uses the caller canvas (512x512 by default) without fitting content bounds. The reference fits the roughly 200-unit artwork to 500 pixels. Render-only; caller-canvas geometry has an independent assertion. |
| `font/simple-case` | The PNG is 400x400, so the harness preserves that explicit canvas. The unitless `font` presentation shorthand is a Donner extension and matches expanded longhands. The hermetic set has no BoldItalic face: style-first matching selects NotoSans-Italic at weight 400. Glyph weight/advance still differ from the reference in both CPU text tiers; synthetic-weight/reference qualification remains with #1167/#1173. Render-only. |

[SVG 2 radial-gradient rules](https://www.w3.org/TR/SVG2/pservers.html#RadialGradients) and
[error processing](https://www.w3.org/TR/SVG2/conform.html#ErrorProcessing) establish the radial
error and containing-circle expectations. The independent tests preserve visible sibling artwork
and a valid zero-radius last-stop control. Both backends reject negative radii before solid-color
shortcuts, including single-stop gradients and an outer radius within the CPU near-zero tolerance.
[CSS Color 4](https://www.w3.org/TR/css-color-4/#rgb-functions) defines the RGB syntax;
[SVG 2 paint grammar](https://www.w3.org/TR/SVG2/painting.html#SpecifyingPaint) does not include the
old `icc-color()` suffix. The `arcs`, unitless font shorthand, caller canvas and zero-radius controls
are explicitly project compatibility contracts, not claims of broader SVG 2 conformance.

The focused regression targets are `renderer_regression_tests`, `renderer_regression_tests_geode`
and `renderer_regression_tests_text_full`. The corpus targets are `resvg_test_suite_default_text`,
`resvg_test_suite_geode` and `resvg_test_suite_max`. The existing Geode corpus text capability gate
still skips the text sample; the independent Geode font contract test executes directly. Render-only
corpus execution establishes stability, not pixel agreement with the historical reference.

## Out of scope (correctly skipped — do not "fix")

| Category                        |          Tests | Why                                                                                                                                                                               |
| ------------------------------- | -------------: | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| filters/enable-background       |             21 | Category default `Params::RenderOnly(...)`: deprecated in SVG 2 (→ `<filter>` chains / `backdrop-filter`). See [`unsupported_svg1_features.md`](../unsupported_svg1_features.md). |
| filters/filter `in=Background*` |              2 | Same deprecation (BackgroundImage/BackgroundAlpha inputs).                                                                                                                        |
| text/tref                       | 9 (+1 display) | `<tref>` removed in SVG 2.                                                                                                                                                        |
| text/kerning                    |              2 | `kerning` attribute deprecated SVG 1.1.                                                                                                                                           |
| text/glyph-orientation-*        |              2 | deprecated SVG 1.1.                                                                                                                                                               |
| paint-servers/radialGradient    |              0 | No skipped focal-radius case: `fr=0.7` is render-only with the containing-circle oracle above. `focal-point-correction` compares normally.                                                                       |
| structure/style-attribute       |              1 | `<svg version="1.1">` disables geometry-in-style (SVG 1.1 behavior).                                                                                                              |
| Other RenderOnly UB cases       |             51 | Implementation-defined output; we verify no-crash only (per project policy, kept RenderOnly not Skip).                                                                            |

---

## Geode / Resvg Override Policy

Geode is part of the same resvg test matrix as the CPU variants. It should use the
same `ImageComparisonParams` thresholds, render-only state, skips, and golden
overrides as the other renderers. Build capability is recorded through normal
`Params` feature requirements, never through parity-only backend disables or side-table gates.

Policy:

- Do not add `geodeCategoryGate`, `geodeFilenameGate`, or backend-specific threshold
  side tables.
- Do not maintain symptom-ledger sets such as `kEdgeFloor` or `kGenuineG2` in the
  resvg file. If an independently verified analytic Geode result differs from the shared reference,
  attach an exact local `withGeodeGoldenOverride(...)` and current reason to that case. Never disable
  the backend or widen a Geode-only threshold to absorb the difference.
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
caution: the analytic rewrite left those tests **byte-identical**, _proving_ coverage was
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
No active resvg case is backend-disabled. Verified analytic-vs-finite-sample edge
residuals, including `feGaussianBlur/complex-transform`, paint-order text, and the
rotated pattern case, use exact Geode goldens so the backend remains fully gated.

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
