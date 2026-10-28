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

| State                                                      |    Call sites | Meaning                                                                                                                                                                                  |
| ---------------------------------------------------------- | ------------: | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Params::Skip("reason")`                                   |            77 | Not run. Feature gap, known bug, reference disagreement, harness mismatch or API policy. The bulk of this doc.                                                                           |
| `Params::UndefinedBehavior("reason")`                      |            51 | Rendered, **not** compared. Undefined or implementation-defined behavior; see the policy below.                                                                                          |
| `Params::DeprecatedInSVG2("reason")`                       | 23 (39 cases) | Rendered, **not** compared. Feature removed from or deprecated in SVG 2 that Donner does not implement.                                                                                  |
| Commented-out `INSTANTIATE_TEST_SUITE_P`                   |             0 | Every vendored category is instantiated.                                                                                                                                                 |
| `Params::WithThreshold(…, maxPx)` / local max-pixel budget |           103 | Passes with an explicit threshold or pixel budget. Large non-text budgets remain suspect; see [Masked bugs behind inflated CPU thresholds](#masked-bugs-behind-inflated-cpu-thresholds). |
| Geode-disabled local `Params` entries                      |             0 | Geode now runs every active resvg case. Verified analytic edge residuals use exact per-backend goldens instead of disabling the backend or inflating thresholds.                         |

**Rendering without a comparison has two named buckets.** A case that renders without a pixel
comparison hides every regression in its output, so it needs one of two factories:

- `Params::UndefinedBehavior(reason)` marks behavior the specification leaves undefined or
  implementation-defined, or a case whose vendored reference is the resvg UB marker image
  (`third_party/resvg-test-suite/resources/ub.png`, SHA-256
  `131f32bfb5bcdb78b9e97021ee94fa0f6c5cb4564254a15bc02add2cedb168e2`). Its reference must be the
  marker unless `SPEC_UB_ALLOWLIST` in `tools/resvg_parity/parity_report.py` lists the case with
  its specification-level reason (today only `feGaussianBlur/huge-stdDeviation`, an
  implementation-defined blur resource limit).
- `Params::DeprecatedInSVG2(reason)` marks a feature that SVG 2 removed or deprecated and Donner
  deliberately does not implement, such as `enable-background`, `<tref>`, the `kerning` property
  or `icc-color()` paint. Each case must fall under a feature named in
  `DEPRECATED_IN_SVG2_FEATURES` in the same file, so this bucket cannot absorb bugs. A case whose
  subject is a removed feature uses this bucket even when its reference is the UB marker.

`//tools/resvg_parity:parity_report_tests` enforces both rules, and the parity report counts the
two buckets separately. Both still render, so they keep no-crash and sanitizer coverage. Donner
bugs and missing features, reference disagreements where Donner follows the specification,
harness or background mismatches, and API policy differences are `Params::Skip` with a bug number
or reason; skipped cases do not render at all. A UB marker reference does not by itself make a
case undefined behavior: a `Not impl:` or `Deferred:` feature on a marker reference stays a skip.
A case that passes the default comparison compares. The parity report counts 90 render-only cases
(51 undefined behavior: 50 on the UB marker reference plus the allowlisted case; 39 deprecated in
SVG 2), 77 skipped cases and 1512 compared cases.

## Current totals

|                                       |                                                                                                                                                      Count |
| ------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------: |
| `Params::Skip(...)`                   |                                                                                                                             77 (`grep -o 'Params::Skip('`) |
| `Params::UndefinedBehavior(...)`      |                                                                                                     51 (render-must-not-crash, no pixel compare; 51 cases) |
| `Params::DeprecatedInSVG2(...)`       |                                                                        23 (render-must-not-crash, no pixel compare; 39 cases including a category default) |
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
| B2  | `filters/filter-functions` coverage                                     | **DONE** (27/43 files compare) | All cases execute; 16 reference qualifications have independent contract tests.                                                                                                                                           |
| B3  | `structure/image` golden kernel-era mismatch                            |                       **DONE** | Mitchell cubic for quality `<image>` rendering and v0.49.0 raster references (see [B3](#b3-structureimage-golden-kernel-era-mismatch)) |
| F12 | `transform-origin` on `<textPath>` baseline                             |                       **DONE** | Resolved by #868; all category cases are active.                                                                                                                                                    |
| F7  | `paint-order` rendering                                                 |               **DONE** (14/14) | Shapes and text run on both backends; `on-tspan` uses a project-owned oracle because the vendored PNG breaks cross-span kerning.                                                                    |
| F9  | `textLength` + `lengthAdjust` stretch/compress                          |                       **DONE** | Interaction cases compare; `textLength/zero` compares with a reference rasterizer generation budget (see [F9](#f9-textlength--lengthadjust)).                                                         |
| F10 | Remaining `textPath` vertical, kerning, and filter cases                |                              4 | Feature                                                                                                                                                                                             |
| F11 | BiDi / RTL text shaping                                                 |                             ~8 | Feature (needs `text-full`)                                                                                                                                                                         |
| B7  | text reference qualification                                            |                              2 | Allowances removed; color-adjust labels compare with generation budgets; `font/simple-case` and `fallback-1` remain skipped (see [B7](#b7-text-reference-qualification)) |
| —   | masking edge cases (mask 3, clipPath 5)                                 |                              8 | `mask-type` and vector text clip children are active on both backends; remaining cases are units, transforms, color interpolation, bitmap text clips, nested intersections, and shorthand geometry. |
| —   | uncertain `Bug?` entries (need triage)                                  |                            ~12 | Needs investigation                                                                                                                                                                                 |
| F1  | `enable-background` + `in=Background*`                                  |                             23 | **Out of scope** (deprecated)                                                                                                                                                                       |
| —   | other deprecated/UB skips                                               |                            ~30 | **Out of scope**                                                                                                                                                                                    |

---

## Tracked regressions & disabled blocks

### B2: `filters/filter-functions` category coverage

All 43 fixtures execute. Twenty-eight retain ordinary comparisons, including `blur-function` and
`drop-shadow-function-filter-region`; fifteen have explicit reference qualifications and independent
rendering assertions. Most use `CssFilterContract*`; the inherited invalid-URL case uses
`FilterReferenceStages`. No threshold or golden is changed.

| Assigned cases | Disposition |
| --- | --- |
| Seven `color-adjust-functions-*` cases | Compare with a 390-pixel reference label glyph budget. Every mismatch is in label glyphs: 354 pixels in default text, 370 in full text, 375 in Geode. Filtered geometry agrees. Independent endpoints, omitted/default values, percentages, clamping and invalid-negative rejection pass. The labels select Noto Sans Regular (see [B7](#b7-text-reference-qualification)). |
| Five remaining `drop-shadow-function*` cases | Reference disagreement. Each reference shadow is the Donner shadow moved to a truncated offset: rendering Donner at that offset matches within the default allowance (21 to 72 pixels, against 2,513 to 6,582 at Donner's own offset). For the three `4 5 6` cases the device offset is (10, 12.5); the reference truncates it to (10, 12), while Donner rounds to (10, 13) under its half-pixel rounding contract, and an exact half-pixel shadow still differs from the reference by 447 pixels. For `em-values` and `mm-values` the reference shadows sit at device offsets (30, 47) and (27, 37) instead of the specified (32, 48) and (28.35, 37.80). Reproducing them needs two truncations: the resolved user lengths to whole units before scaling (12.8 19.2 to 12 19, and 11.34 15.12 to 11 15), a hypothesis the current usvg source does not show, and the scaled offset toward zero, which resvg's `apply_drop_shadow` does with `dx as i32`. Color order, currentColor, em/mm resolution and alpha compare independently. |
| `two-exact-urls`                             | Valid repeated URL syntax; PNG bytes equal the corpus UB marker. Repeated bounded offsets match their explicit composed offset. Separate blur references retain intermediate clipping and sampling, so they need not equal two blur primitives in one graph.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| `two-urls`                                   | Compares by default. The boundary difference was Donner's: the three box passes that approximate the Gaussian discarded what the earlier passes spread past the filter raster edge, which the later passes must read back. TinySkia now runs the passes on zero-padded lines, and Geode evaluates the three passes as one cascade kernel per axis that samples the input directly, so a blur near the raster edge equals the same blur computed with room around it.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| `url-and-grayscale`                          | CSS functions use sRGB. An independent solid-color oracle gives 115 for the sample's encoded-channel luminance; the historical reference center is 122, consistent with linearRGB processing.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| `one-invalid-url-in-list`                    | An invalid URL disables the entire chain rather than preserving the prior stage. `FilterReferenceStages.InvalidReferenceDisablesEntireChain` independently covers that behavior.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |

[Filter Effects](https://www.w3.org/TR/filter-effects-1/#FilterFunctions) defines function grammar,
sRGB processing and chaining. Drop-shadow now rejects nonzero unitless CSS lengths while preserving
SVG presentation-attribute user units and unitless zero. Negative standard deviations reject the
whole declaration. Its color alpha is applied once; flood-opacity is a separate factor. Focused
regressions first reproduce invalid declarations changing output and translucent shadows receiving
alpha twice, then pass on TinySkia, full text and Geode. The full property suite also checks that
rejected declarations preserve the prior value at the same cascade priority.

The named gates are `properties_tests`, `renderer_driver_tests`, the three
`renderer_regression_tests` configurations and all three resvg corpus targets. A raster sanitizer
seed covers valid translucent shadows and invalid declarations. The qualified corpus cases are
skips that name their cause: label-glyph differences under #1167, and drop-shadow references whose
offsets are truncated, which are reference disagreements. #1169 tracks these classifications.
`two-urls` now compares by default after the box blur edge fix; the remaining filter-region and
sampling cases stay with #1170.

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
they are reference-disagreement skips backed by visible-geometry assertions, documented
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

## CSS radial clipping

`clip-path: circle()` and `ellipse()` support length, percentage and
`closest-side`/`farthest-side` radii, CSS positions,
and fill/stroke/view reference boxes. Default SVG border boxes use stroke geometry. Reference
geometry includes image occurrences and horizontal text, including hidden glyphs for stroke bounds.
Work admission precedes path scans and glyph outline decoding. Vertical text fill/stroke reference
boxes are currently refused; viewport-based circles remain available. This does not claim a hard
allocation bound inside the existing text shaper.

Both renderers apply radial shapes to entire groups and preserve them during retained masked-child drag.
Independent live-bitmap comparisons cover boxes, nested clips, occurrence styles, images, text spans,
non-scaling strokes and style mutation; CSS declaration lengths require units while presentation
attributes retain user units.

Ellipse radii resolve independently: horizontal percentages use reference width and vertical
percentages use reference height. A zero radius on either axis produces an empty clip. The
`circle_clip_path_tests` and `properties_tests` targets cover ellipse parsing, reference boxes,
independent axis resolution, transformed/nested clips and invalid declaration fallback. Inset,
polygon and path CSS clipping functions remain unsupported.

The plain `circle-shorthand.svg` comparison is restored. The stroke-box and view-box PNGs use an
opaque white backdrop, while Donner's RGBA API retains transparency outside painted content.
[SVG rendering contexts](https://www.w3.org/TR/SVG2/render.html) permit different final backdrops.
Compositing diagnostic copies of both actual and reference images over white produces zero
pixelmatch mismatches on both backends under the unchanged corpus policy. Strict identity still
differs at raster edges, so this is a background qualification, not a bit-identity claim. The original
PNGs and tolerances remain unchanged; these two fixtures render normally without comparing their
opaque references, and authored geometry comparisons continue to use strict identity.

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

### B7: text reference qualification

**Status:** the font-family allowances are gone. Every `text/font-family/` case
except `fallback-1` compares at default settings in both CPU text tiers, as do
`text/text/xml-lang=ja`, `text/letter-spacing/non-ASCII-character` and
`structure/defs/style-inheritance-on-text`. Face
selection for these cases is asserted directly in `FontSelection_tests.cc`.

- **Selection and loading:** the generic families map to the faces upstream's
  harness uses (Noto Serif, Noto Sans, Noto Mono, Yellowtail, Sedgwick Ave
  Display). The simple tier now reads GPOS pair kerning wrapped in Extension
  lookups, which Source Sans Pro uses exclusively; `font-list` and
  `source-sans-pro` lost their 1300-pixel simple-tier budgets.
- **Stale references:** the generic-family PNGs, `xml-lang=ja.png` and
  `fallback-1.png` were drawn in faces outside the vendored set or for a
  different layout, and `letter-spacing/non-ASCII-character` named its family
  unquoted. They were refreshed from resvg v0.49.0 (provenance in
  `third_party/resvg-test-suite/BUILD.bazel`). The refreshed `xml-lang=ja` draws
  all three lines in Mplus 1p, the only vendored face covering them, so it no
  longer exercises language-specific face selection.
- **Weights 400 and 700 only:** the comparison fixture registers only these
  weights. The snapshot's `font-weight` references draw weights 100 to 300 with
  Noto Sans Regular and 900 with Noto Sans Bold, and Donner matches them with zero
  differing pixels. resvg v0.49.0 draws 900 with Noto Sans Black, so registering
  the other weights belongs with a refresh of those references.
- **Color-adjust labels:** the seven `color-adjust-functions-*` labels are 8-unit
  Noto Sans Regular, and their pixels are identical across all seven references in
  each reference set. Against the vendored 500x500 PNGs they differ by 354 simple,
  370 full and 375 Geode pixels, all inside the three label bands; those PNGs carry
  about 14% more label ink than Donner's Noto Sans Regular. Against resvg v0.49.0's
  300x300 rendering label ink agrees within 2%, and a 4x4 supersampled prototype of
  Donner's rasterizer brings the residual from 364 simple and 377 full pixels to 5
  and 52. They compare with a 390-pixel label glyph budget (see
  [Reference label glyph budgets](#reference-label-glyph-budgets)).
- **Remaining:** `text/font/simple-case` disagrees on the unitless
  `font` shorthand (see the reference qualification table). `fallback-1` is a
  harness mismatch: resvg's harness falls back to Noto Serif for an unknown
  family and Donner to its embedded Public Sans (445 simple, 444 full pixels).

> The remaining sub-1000-px CPU thresholds (feColorMatrix matrix/saturate variants,
> feDropShadow, text-decoration rotate-lists, pattern AA) are small enough to be
> plausible coverage-geometry differences; audit opportunistically but they are not
> promoted bugs.

### Reference rasterizer generation budgets

The vendored references come from more than one resvg rasterizer generation. Newer PNGs cover
edges with tiny-skia's 4x4 supersampling, so partial alphas fall on 1/16 steps; older ones match
Donner's analytic coverage. The difference is largest on the 1.25-pixel crosshair most text tests
draw at x=250: analytic coverage splits it 160/160 across two columns, 4x4 coverage 128/192, so a
whole reference can differ by about 400 pixels with identical layout. Donner keeps analytic
coverage on both the CPU and GPU backends, so these references cannot match exactly.

A case may carry a pixel budget for this cause, with a reason starting `Reference rasterizer
generation:`, only when all of the following hold:

- A 4x4 supersampled prototype of the CPU rasterizer, measured against the same reference,
  compares within the default allowance (0.02 threshold, 100 pixels).
- The analytic residual sits on edge coverage (the crosshair columns or glyph edges); the layout
  matches.
- The budget is a per-case pixel count at the default color threshold: the largest residual
  measured on both CPU text tiers on macOS and Linux (and Geode where it runs), plus 3% rounded up
  to 5 pixels. The reason states the analytic and the supersampled residual.
- The detection a deliberate regression gets is measured and recorded: shifting the text 1 pixel
  exceeds the budget in every case except `hanging`, below.

| Case | Before | Residual (px) | 4x4 (px) | Budget | 1 px text shift |
| --- | --- | ---: | ---: | ---: | ---: |
| `text/font/font-shorthand` | skip | 386 | 0 | 400 | 422 |
| `text/text/real-text-height` | skip | 397 | 0 | 410 | 441 |
| `text/textLength/zero` | skip | 396 | 0 | 410 | 433 |
| `text/text/ligatures-handling-in-mixed-fonts-1` (full text) | skip | 399 | 0 | 415 | 507 |
| `text/text/ligatures-handling-in-mixed-fonts-2` (full text) | skip | 343 | 0 | 355 | 435 |
| `text/baseline-shift/nested-with-baseline-1`, `-2` | 0.1 / 100 | 384 | 0 | 400 | 408 |
| `text/dominant-baseline/hanging` | 0.1 / 100 | 337 | 0 | 350 | 344 |
| `text/tspan/nested-whitespaces` | 400 | 387 | 0 | 400 | 416 |
| `text/text/zalgo` (full text) | 300 | 257 | 1 | 265 | 468 |

The two ligature cases stay gated to full text: the simple tier lacks mixed-font ligature shaping
(1555 and 3134 pixels, #1176).

Moving `nested-with-baseline` and `hanging` from a 0.1 color threshold with 100 pixels to a 0.02
count is not tighter on both axes: the count is larger and the color threshold smaller. It is
justified by what each setting detects:

- `nested-with-baseline`: the 1 px shift counted 22 to 39 pixels under 0.1 / 100 and passed; it
  counts 408 to 425 under the new budget and fails.
- `hanging`, one Devanagari glyph whose glyph-edge differences pixelmatch treats as
  anti-aliasing:

  | Regression | 0.1 / 100 | 0.02 count (budget 350) | 0.02 with anti-aliased pixels |
  | --- | ---: | ---: | ---: |
  | none | 0 | 337 | 940 |
  | 1 px text shift | 6 (passes) | 344 (passes) | 1235 |
  | 2 px text shift | 225 (fails) | 562 (fails) | 1465 |
  | `dominant-baseline="alphabetic"` | 2714 (fails) | 2997 (fails) | not recorded |

  Both settings catch the baseline change the test is about and a 2 px shift; neither catches a 1
  px shift. Counting anti-aliased pixels would catch it (940 against 1235), but makes the budget a
  measure of glyph-edge coverage rather than of the crosshair, so it is not used.

`//tools/resvg_parity:parity_report_tests` checks the form of this class and the next. Such a
compared case:

- comes from a per-case override, not a category default;
- has a pixel budget with no other exception (no simple-text, Geode or golden override);
- keeps the default color threshold;
- has a budget at most its stated analytic residual plus 3%, rounded up to 5 pixels;
- states its supersampled residual, which for this class must be within the default allowance.

The parity report counts them as `reference_generation_budget_cases` and
`reference_label_glyph_budget_cases`.

### Reference label glyph budgets

The seven `filters/filter-functions/color-adjust-functions-*` cases fail the first condition. They
differ from their vendored 500x500 PNGs by 354 simple, 370 full and 375 Geode pixels, all inside
the three label bands; the filtered squares compare exactly. Under 4x4 coverage the difference is
still 302 and 297 pixels, because those PNGs draw the 8-unit Noto Sans labels about 14% heavier.
Against resvg v0.49.0's rendering, 4x4 coverage brings the labels to 5 and 52 pixels.

They compare with a 390-pixel budget whose reason starts `Reference label glyphs:` and states the
analytic and supersampled residuals. Detection was measured on the residual-bearing PNGs:

| Regression | Pixels (simple / full) | Budget |
| --- | ---: | ---: |
| none | 354 / 370 | 390 |
| 1 px label shift | 1508 / 1524 | fails |
| brightness 50% to 45% on one square | 10354 / 10370 | fails |
| opacity 50% to 45% on one square | 5358 / 5374 | fails |
| sepia 2 to 0.9 on one square | 10354 / 10370 | fails |
| grayscale 2 to 0.9 on one square | 354 / 370 | passes: no pixel moves past the 0.02 threshold |

The grayscale change is invisible to any comparison at the default color threshold, budgeted or
not.

### Removed budgets

Budgets that already passed at default settings were removed from 39 cases, including the
`structure/transform-origin` category default (0.13 threshold, 300 pixels; `on-image`,
`on-text-path` and `on-text` still need it and keep it per case), `text/text/xml-space` (1400
pixels) and `painting/fill/pattern-on-text` (2100 pixels). Measured at default settings, their
largest residuals are:

- 54 pixels on either CPU tier, macOS and Linux alike;
- 14 pixels on Geode with Metal on macOS;
- 0 to 71 pixels on Geode with software Vulkan (llvmpipe) on Linux for the `transform-origin`
  rows, with `bBox-impact` and `tiny-pattern-upscaled` also passing at default settings there.

Geode also passes with hardware Vulkan on Linux.

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

**Resolved (2026-10-26).** Placement, intrinsic sizing, MIME sniffing, GIF decode and
`preserveAspectRatio` alignment were already correct; the twelve skipped rows and
`painting/image-rendering/optimizeSpeed` differed only in resampling. A pinned reproduction
attributed every snapshot reference to one of four upscalers, each matched to 0 to 55 differing
pixels at the default threshold:

- `no-height-non-square`, `width-and-height-set-to-auto`: resvg's tiny-skia `Bicubic`, the
  Mitchell-Netravali cubic (B = C = 1/3) over premultiplied sRGB texels.
- `no-height`, `no-width`, `no-width-and-height`, `embedded-jpeg-without-mime` (one shared PNG),
  `embedded-gif`, `external-gif` (another shared PNG): a browser rendering whose bilinear filter
  quantizes subpixel weights to 4 bits.
- `preserveAspectRatio=none` and the three `*-meet` cases: a pre-2019 resvg that resampled each
  image bilinearly to its user-space size before the canvas transform.
- `optimizeSpeed`: a 2019 nearest-neighbor rendering flattened onto white, with ties toward the
  lower texel.

At resvg v0.49.0 every raster-image reference is the Mitchell cubic (nearest for
`optimizeSpeed`). Donner now resamples `<image>` with `image-rendering: auto`, `optimizeQuality`
and `high-quality` through the same cubic on TinySkia (tiny-skia `FilterQuality::Bicubic`) and
Geode (a 16-tap cubic image-blit sampling mode), matching the kernel `feImage` already used;
`smooth` stays bilinear and compositor bitmap blits keep bilinear sampling. resvg renders an
`feImage` raster as an image, so `feImage` placement shares the same `ImageSamplingKernelFor`
mapping on both backends: `smooth` is bilinear there too, and a pure translation samples the
containing texel, as tiny-skia does for `<image>`. That also removed a cubic filtering pass that
rotated `feImage` placement and nested-SVG `feImage` rasters received at one-to-one scale, which
re-enabled `filters/feImage/with-subregion-5` and refreshed the Donner-generated
`filters/feImage/svg` goldens. The raster-image
references were replaced by their v0.49.0 renderings, with provenance in
`third_party/resvg-test-suite/BUILD.bazel`.

The `embedded-svg-with-text` policy case is independently qualified: the vendored reference omits
the embedded SVG's text, while Donner uses the document font catalog. An authored
`EmbeddedSvgUsesDocumentFontContext` case checks embedded-versus-direct identity plus visible text
ink in `//donner/svg/renderer/tests:renderer_public_api_tests` and its
`//donner/svg/renderer/tests:renderer_public_api_tests_geode` variant, so the corpus row is
a reference-disagreement skip rather than a missing feature.

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

**Status:** done. Text stretches or compresses to a target length with `spacing` and
`spacingAndGlyphs`. The `arabic`/`arabic-with-lengthAdjust` cases pass on text-full builds and are
enabled with `.onlyTextFull()`.

With `spacing`, the SVG 2 resolve-text-length loop accrues the per-gap adjustment after every
character of the owner, its last included, but never applies it to later characters; the
specification does not define how following text moves. Chrome adds an explicit shift by the
accrued amount, and the resvg references show the same, so the owner's length still ends at its
last glyph and the text after it starts one gap later. Donner follows them. A nested owner that ends its
parent passes the gap through, and an enclosing owner counts it as fixed advance. Glyph stretching
adds no trailing gap. `textLength/on-a-single-tspan` compares at the default settings, and a
renderer regression checks a fitted span against the same span with an equal `letter-spacing`.

With `spacingAndGlyphs`, a glyph's outline is stretched before its rotation, so a sideways glyph in
vertical text stretches its own x axis and an upright glyph its y axis.
`lengthAdjust/vertical` compares against resvg v0.49.0's 300x300 rendering. The vendored snapshot
placed the sideways glyphs 0.28 units across the line from both Donner and resvg v0.49.0, which is
consistent with a rendering from before the central baseline. Decorations follow the adjusted glyph
extents; `lengthAdjust/with-underline` and `text-decoration/with-textLength-on-a-single-character`
also compare against resvg v0.49.0, because the snapshot drew their underlines at 0.1em, twice the
0.05em thickness in the vendored Noto Sans post table.

`textLength/zero` compares with a 410-pixel budget because its reference comes from a different
rasterizer generation (see
[Reference rasterizer generation budgets](#reference-rasterizer-generation-budgets)).
Its glyph layout matches the vendored reference, where the last glyph's advance ends at x=20. All
396 differing pixels on both CPU tiers are the x=100 crosshair, which this PNG draws at alpha 128
and 192, exactly what 4x4 supersampled coverage produces for its 1.25-pixel stroke, while the
eleven sibling `textLength` references, `lengthAdjust/spacingAndGlyphs` and Donner's analytic
coverage draw it at 160 and 160. resvg
v0.49.0's `zero.png` is not a replacement: its glyphs sit about 17.3 units right, a 1834-pixel
difference.

### F10: `textPath` SVG2 attributes

Inline `path`, geometry references, `side=right`, `method=stretch`, and `spacing=auto` are
implemented. Their focused geometry and renderer contracts are active. The vendored
`method=stretch` image is identical to the align reference, so that corpus row stays skipped while
independent curved-outline tests distinguish the methods. A fragment-free relative `xlink:href`
under the secure static resource policy does not resolve to a same-document element or initiate an
external fetch; its conflicting corpus image is also skipped. The
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

| Category                  | Tests | Gap                                                                                                                                                                                                                                                                                                                                                  |
| ------------------------- | ----: | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| structure/svg             |     0 | `structure/svg/funcIRI-with-quotes.svg` compares with ordinary budgets after the [paint URL parsing fix](../../donner/svg/properties/PropertyRegistry.cc), and `structure/svg/nested-svg-one-with-rect-and-one-with-viewBox.svg` compares with ordinary budgets after the [viewport sizing fix](../../donner/svg/components/layout/LayoutSystem.cc). |
| structure/style           |     1 | CSS `@import` / external CSS remains unsupported: imports are diagnosed and ignored, with no target requests; `UnsupportedCssImportsNeverRequestConfiguredResponses` in `//donner/svg/renderer/tests:renderer_public_api_tests` enforces that.                                                                                                       |
| structure/symbol          |     1 | `transform` on `<symbol>` (SVG2)                                                                                                                                                                                                                                                                                                                     |
| painting/image-rendering  |     1 | Both backends implement the typed values, including two-stage non-integer `pixelated` scaling. `on-feImage` uses an independently derived nearest reference; `optimizeSpeed` compares against resvg v0.49.0 on TinySkia and is disabled on Geode, whose interpolated texture coordinate lands just below texel boundaries on pixel centers.          |
| masking/clipPath          |     1 | CSS circle shapes support fill, stroke and viewport reference boxes with bounded geometry preparation; two opaque reference images retain explicit background qualifications. The transformed-text reference remains under review.                                                                                                                   |
| masking/mask              |     0 | Transformed group and rotated mask regions compare with ordinary budgets; `mask-type` alpha/luminance and mask `color-interpolation` sRGB/linearRGB are active.                                                                                                                                                                                      |
| text/font                 |     2 | `font` shorthand; canvas-size mismatch (test harness)                                                                                                                                                                                                                                                                                                |
| text/tspan                |     3 | tspan interaction with `clip-path`/`filter`/`mask`                                                                                                                                                                                                                                                                                                   |
| painting/stroke-dasharray |     4 | `0 n` dash patterns with caps; `40 0` closed-rect dash-seam (see note)                                                                                                                                                                                                                                                                               |
| painting/marker           |     3 | multiple closepaths, recursive-5 (rounded-rect corner fixed, [#623](https://github.com/jwmcglynn/donner/issues/623))                                                                                                                                                                                                                                 |

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
| text/writing-mode | ~7 | `vertical-lr` / `vertical-rl` edge cases and mixed-script (upright CJK plus rotated Latin) column geometry. Obsolete `tb*` SVG 1.1 values should be classified or mapped to their required compatibility aliases, not implemented as distinct modes. Font fallback is per cluster, so the `mixed-languages-with-tb*` cases draw their Arabic words; vertical anchoring measures chunks between pen positions. They stay skipped because upright CJK sits 0.026em lower than the reference, and the underline case also lacks bidi runs and its vertical underline. |

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
They remain `Params::UndefinedBehavior` corpus cases, with independent `ReferenceContract*`
comparisons in `RendererRegression_tests.cc`. The other assigned fixtures do not use the UB
marker, so they are skips with their reason. No reference bitmap or comparison tolerance is
changed.

| Assigned fixture                               | Supported behavior and reference disposition                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| ---------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `radialGradient/fr=-1`                         | Negative focal radius is invalid and applies no paint. UB marker reference.                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| `radialGradient/fr=0.5`                        | Coincident start/end circles apply no paint. UB marker reference.                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| `radialGradient/fr=0.7`                        | A start circle containing the end circle applies no paint; the reference instead paints a reversed gradient. Reference-disagreement skip, with an independent transparent-paint oracle.                                                                                                                                                                                                                                                                                                                                   |
| `radialGradient/invalid-gradientTransform`     | The authored all-zero matrix is singular, rather than malformed syntax. No paint; UB marker reference.                                                                                                                                                                                                                                                                                                                                                                                                                    |
| `radialGradient/invalid-gradientUnits`         | The invalid enumeration is diagnosed and the initial `objectBoundingBox` value is retained. UB marker reference.                                                                                                                                                                                                                                                                                                                                                                                                          |
| `radialGradient/negative-r`                    | Negative outer radius is invalid and applies no paint. UB marker reference.                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| `fill/rgb-int-int-int`                         | Space-separated RGB is supported CSS Color 4 syntax and matches the equivalent hex color. UB marker reference.                                                                                                                                                                                                                                                                                                                                                                                                            |
| `fill/valid-FuncIRI-with-a-fallback-ICC-color` | The obsolete SVG 1.1 `icc-color()` paint grammar is unsupported. Invalid CSS declarations are rejected, preserving inherited paint; the historical reference expects a gradient. `Params::DeprecatedInSVG2` (removed in SVG 2): rendered, not compared.                                                                                                                                                                                                                                                                   |
| `stroke-linejoin/arcs`                         | The SVG Strokes draft join is not implemented; Donner retains its documented miter fallback. The reference is the UB marker, not an arcs rendering.                                                                                                                                                                                                                                                                                                                                                                       |
| `stroke-linejoin/miter-clip`                   | Separate Geode/editor parity repair and ordinary reference restoration: [#2057](https://github.com/jwmcglynn/donner/pull/2057).                                                                                                                                                                                                                                                                                                                                                                                           |
| `svg/no-size`                                  | An unsized root uses the caller canvas (512x512 by default) without fitting content bounds. The reference fits the roughly 200-unit artwork to 500 pixels. API-policy skip; caller-canvas geometry has an independent assertion.                                                                                                                                                                                                                                                                                          |
| `font/simple-case`                             | The PNG is 400x400, so the harness preserves that explicit canvas. The unitless `font` presentation shorthand is a Donner extension and matches expanded longhands. The hermetic set has no BoldItalic face: style-first matching selects NotoSans-Italic at weight 400. The vendored PNG synthesizes bold over that face with wider advances, which neither CPU text tier does for document fonts, while resvg v0.49.0 rejects the unitless size and draws 12px default serif text. Reference-disagreement skip (#1167). |

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
corpus execution of the UB-marker cases establishes stability, not pixel agreement with the
historical reference.

## Out of scope (correctly not compared; do not "fix")

| Category or case                                                          | Tests | State and why                                                                                                                                                                                                                                                                     |
| ------------------------------------------------------------------------- | ----: | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| filters/enable-background                                                 |    17 | Category default `Params::DeprecatedInSVG2(...)`: removed in SVG 2 (→ `<filter>` chains / `backdrop-filter`). The other four of the 21 cases already match the reference with SVG 2 semantics and compare. See [`unsupported_svg1_features.md`](../unsupported_svg1_features.md). |
| filters/filter `in=Background*-with-enable-background`                    |     2 | `Params::DeprecatedInSVG2`: the BackgroundImage/BackgroundAlpha snapshot comes from `enable-background`.                                                                                                                                                                          |
| text/tref, painting/display/none-on-tref                                  | 9 + 1 | `Params::DeprecatedInSVG2`: `<tref>` removed in SVG 2.                                                                                                                                                                                                                            |
| text/kerning                                                              |     2 | `Params::DeprecatedInSVG2`: `kerning` property removed in SVG 2.                                                                                                                                                                                                                  |
| text/glyph-orientation-horizontal                                         |     1 | `Params::DeprecatedInSVG2`: removed in SVG 2. `glyph-orientation-vertical` is implemented and keeps a residual skip.                                                                                                                                                              |
| painting/fill `icc-color`, `valid-FuncIRI-with-a-fallback-ICC-color`      |     2 | `Params::DeprecatedInSVG2`: SVG 1.1 `icc-color()` paint is not in the SVG 2 paint grammar.                                                                                                                                                                                        |
| structure/style-attribute, structure/style `non-presentational-attribute` |     2 | `Params::DeprecatedInSVG2`: `<svg version="1.1">` semantics; SVG 2 removed the `version` attribute.                                                                                                                                                                               |
| masking/clip                                                              |     1 | `Params::DeprecatedInSVG2`: the CSS `clip` property is deprecated by CSS Masking in favor of `clip-path`.                                                                                                                                                                         |
| text/dominant-baseline `reset-size`, `use-script`                         |     2 | `Params::DeprecatedInSVG2`: SVG 1.1 values removed in SVG 2; UB marker references.                                                                                                                                                                                                |
| paint-servers/radialGradient                                              |     1 | `fr=0.7` is a reference-disagreement skip with the containing-circle oracle above. `focal-point-correction` compares normally.                                                                                                                                                    |
| Other `Params::UndefinedBehavior` cases                                   |    51 | Undefined or implementation-defined behavior: 50 on the resvg UB marker reference plus `feGaussianBlur/huge-stdDeviation`; we verify no-crash only.                                                                                                                               |

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
- Category-wide defaults are acceptable when the default reason applies to every file not
  explicitly overridden. `filters/enable-background` is the model: one category default
  `Params::DeprecatedInSVG2(...)`, not a per-file list, with overrides only for the cases that
  compare. Category feature requirements
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
