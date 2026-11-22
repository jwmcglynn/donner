# resvg-test-suite: Feature Gaps & Open Bugs

**Status:** Living catalog. The CPU-backend (RendererTinySkia) feature gaps and bugs
are the active front, and lead this backlog. Geode runs the same resvg `Params`
and thresholds as the CPU variants; backend-specific override tables are not part
of the suite policy.

The triage backlog for [0022](0022-resvg_test_suite_upgrade.md)'s Milestone 2 —
working through the tests the suite upgrade pulled in and either fixing the
underlying gap or recording the registration that fits it (a Donner-rendered golden for a
reference disagreement, or an allowlisted `Params::Skip(...)` for an unimplemented feature). Each
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

The suite records explicit skips, render-only policies, comparison settings, and
capability requirements through the normal `Params` path close to the cases.
A disabled registration is tracked separately from an active case. A numeric
override can tighten an assertion or preserve a measured residual.

**Rendering without a comparison has two named buckets.** A case that renders without a pixel
comparison hides every regression in its output, so it needs one of two factories:

- `Params::UndefinedBehavior(reason)` marks behavior the specification leaves undefined or
  implementation-defined, or a case whose vendored reference is the resvg UB marker image
  (`third_party/resvg-test-suite/resources/ub.png`, SHA-256
  `131f32bfb5bcdb78b9e97021ee94fa0f6c5cb4564254a15bc02add2cedb168e2`). Its reference must be the
  marker unless `SPEC_UB_ALLOWLIST` in `tools/resvg_parity/parity_report.py` lists the case with
  its specification-level reason (today only `feGaussianBlur/huge-stdDeviation`, an
  implementation-defined blur resource limit).
  A marker records the absence of a qualified upstream pixel reference. The report keeps that
  provenance separate from normative classification. Known valid examples such as the tile
  transform and stitched turbulence retain no-crash coverage with a defined-input disposition.
- `Params::DeprecatedInSVG2(reason)` marks a feature that SVG 2 removed or deprecated and Donner
  deliberately does not implement, such as `enable-background`, `<tref>`, the `kerning` property
  or `icc-color()` paint. Each case must fall under a feature named in
  `DEPRECATED_IN_SVG2_FEATURES` in the same file, so this bucket cannot absorb bugs. A case whose
  subject is a removed feature uses this bucket even when its reference is the UB marker.

`//tools/resvg_parity:parity_report_tests` enforces both rules, and the parity report counts the
two buckets separately. Both still render, so they keep no-crash and sanitizer coverage.

**Reference disagreements get a golden, not a skip.** Where Donner follows the specification and the
vendored reference does not, where the harness and the reference disagree (for example an opaque
reference background), and where an API policy differs, the case compares against a Donner-rendered
golden with a recorded justification: a `Params::WithGoldenOverride` reason, an entry in
`tools/resvg_parity/exception_policies.json`, and a golden README row with the file's SHA-256. A
skip is only for an unimplemented feature with an issue number (`Not impl: <feature> (#<issue>)`);
skipped cases do not render at all. `SKIP_ALLOWLIST` in `parity_report.py` lists every active skip
and may only shrink, so a new skip needs maintainer approval. The one skip it lists is an
unsupported input encoding. Pixel budgets
in the hundreds are not accepted: fix the engine, or hold the row back until it is fixed.
`PIXEL_BUDGET_ALLOWLIST` and `THRESHOLD_ALLOWLIST` record the existing budgets above 100 pixels and
color thresholds above 0.05 at their current values and may only shrink. A UB marker reference does
not by itself make a case undefined behavior: an unimplemented feature on a marker reference stays a
skip. A case that passes the default comparison compares.

## Current totals

The report keeps static registrations separate from measured test results. Numeric
settings, reference paths and replacement provenance, configured states, and
observed outcomes are available per lane. The [exception catalog](../../tools/resvg_parity/exception_policies.json)
records scoped evidence, the issue or disposition, and the condition for removing each
skip, reference override, or comparison allowance. Named source policies cover
render-only cases and capability gates. The exact zero-difference dash seam remains
a stronger pixel assertion.

Regenerate this inventory with
`python3 tools/resvg_parity/parity_report.py --inventory-markdown`.
`//tools/resvg_parity:parity_report_tests` checks the generated block against this document.
The lane counts below are eligibility from source. `geode` compares the whole corpus on Geode in the
simple text tier, and `geode_max_text` compares the text-rendering cases
(`//third_party/resvg-test-suite:tests_with_text`) on Geode with full shaping; `default_text` and
`max` are the CPU lanes of the two tiers. Rows that exceed their Geode budget only because of the
references' quarter-pixel edge quantization opt into the edge coverage rule
(`QUARTER_PIXEL_EDGE_ALLOWLIST`), which accepts only partially covered pixels whose upper or lower
neighbor in the reference is empty or fully covered, and neither of which has the pixel's alpha. A
row whose custom golden is a Donner software rendering, recorded as quarter-pixel quantized in the
golden README, may opt in the same way. The rows in `GEODE_GATE_ALLOWLIST` disable their Geode
comparison with a recorded reason. Both lists are in `parity_report.py` and described in
`README_resvg_test_suite.md`. A gate disables the Geode comparison in both text tiers, or only in
the simple tier when its cause is limited to that tier. A row that is gated or capability-gated on
one lane is not counted among that lane's comparisons.

<!-- BEGIN RESVG INVENTORY -->

| Registered SVG fixtures | Count |
| --- | ---: |
| Total | 1,679 |
| Pixel comparisons | 1,580 |
| Comparisons with a numeric setting or custom golden | 78 |
| Render-only, undefined-behavior registration | 55 |
| Render-only, legacy feature | 43 |
| Explicit skips | 1 |
| Fixtures in disabled categories | 0 |

| Configured lane | Pixel comparisons | Render-only | Capability-gated | Explicit skips |
| --- | ---: | ---: | ---: | ---: |
| `default_text` | 1,554 | 98 | 26 | 1 |
| `max` | 1,580 | 98 | 0 | 1 |
| `geode` | 1,549 | 98 | 21 | 1 |
| `geode_max_text` | 360 | 30 | 0 | 1 |

<!-- END RESVG INVENTORY -->

### Reading observed coverage

The report defaults every outcome to `unobserved`. Use per-shard GoogleTest XML
to report what ran. Supply one XML file or a directory of disjoint receipt files
for a lane. Each import requires the revision of the executed source. For example:

```sh
rev=$(git rev-parse HEAD)
python3 tools/resvg_parity/parity_report.py \
  --execution "$rev:default_text=reports/simple" \
  --execution "$rev:max=reports/full" \
  --execution "$rev:geode=reports/geode" \
  --execution "$rev:geode_max_text=reports/geode-full"
```

Use `--revision` with a source archive or a dirty checkout to supply a caller
assertion about the executed source. Each imported result records the caller's
revision assertion and the XML hashes. XML reports a test result, and cannot by
itself prove that the test executed a pixel comparison. For example, a golden
update also completes successfully. Unlisted cases stay unobserved. A suppressed
test is reported separately from a run that emitted a runtime skip.

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
| B2  | `filters/filter-functions` coverage                                     | **DONE** (34/43 files compare) | All cases execute; 9 reference qualifications have independent contract tests.                                                                                                                                            |
| B3  | `structure/image` golden kernel-era mismatch                            |                       **DONE** | Mitchell cubic for quality `<image>` rendering and upstream resvg raster references (see [B3](#b3-structureimage-golden-kernel-era-mismatch)) |
| F12 | `transform-origin` on `<textPath>` baseline                             |                       **DONE** | Resolved by #868; all category cases are active.                                                                                                                                                    |
| F7  | `paint-order` rendering                                                 |               **DONE** (14/14) | Shapes and text run on both backends; `on-tspan` uses a project-owned oracle because the vendored PNG breaks cross-span kerning.                                                                    |
| F9  | `textLength` + `lengthAdjust` stretch/compress                          |                       **DONE** | Interaction cases compare; `textLength/zero` compares against upstream resvg's reference at default settings (see [F9](#f9-textlength--lengthadjust)).                                                 |
| F10 | `textPath` vertical case                                                |                       **DONE** | Compares against a Donner golden at default settings (see [F10](#f10-textpath-svg2-attributes)).                                                                                                    |
| F11 | BiDi / RTL text shaping                                                 |                             ~8 | Feature (needs `text-full`)                                                                                                                                                                         |
| B7  | text reference qualification                                            |                              2 | Allowances removed; color-adjust labels compare at default settings; `font/simple-case` compares against a Donner golden (see [B7](#b7-text-reference-qualification))                 |
| —   | masking edge cases (mask 3, clipPath 5)                                 |                              8 | `mask-type` and vector text clip children are active on both backends; remaining cases are units, transforms, color interpolation, bitmap text clips, nested intersections, and shorthand geometry. |
| —   | uncertain `Bug?` entries (need triage)                                  |                            ~12 | Needs investigation                                                                                                                                                                                 |
| F1  | `enable-background` + `in=Background*`                                  |                             23 | **Out of scope** (deprecated)                                                                                                                                                                       |
| —   | other deprecated/UB render-only cases                                               |       See the current inventory | **Out of scope**                                                                                                                                                                                    |

---

## Tracked regressions & disabled blocks

### B2: `filters/filter-functions` category coverage

All 43 fixtures execute. Thirty retain ordinary comparisons, including `blur-function`,
`drop-shadow-function-filter-region` and `url-and-grayscale`; four compare against Donner goldens,
and nine have explicit reference qualifications and independent rendering assertions. Most use `CssFilterContract*`; the inherited invalid-URL case uses
`FilterReferenceStages`.

| Assigned cases | Disposition |
| --- | --- |
| Seven `color-adjust-functions-*` cases | Compare at the default threshold and 100-pixel limit on both backends. The references retain the upstream filter squares and frame. The labels use independently shaped Noto Sans outlines (see [B7](#b7-text-reference-qualification)). |
| Four `drop-shadow-function*` cases | Compare against Donner goldens (see the [golden README](../../donner/svg/renderer/testdata/golden/README.md)). Each reference shadow is the Donner shadow moved to a truncated offset: rendering Donner at that offset matches within the default allowance (21 to 72 pixels, against 2,513 to 6,582 at Donner's own offset). For `drop-shadow-function` and `color-last` the device offset is (10, 12.5); the reference truncates it to (10, 12), while Donner translates the shadow by the exact offset and resamples the half pixel. For `em-values` and `mm-values` the reference shadows sit at device offsets (30, 47) and (27, 37) instead of the specified (32, 48) and (28.35, 37.80), consistent with the resolved user lengths truncated to whole units before scaling and the scaled offset truncated toward zero. Color order, currentColor, em/mm resolution and alpha compare independently. |
| `two-exact-urls`                             | Valid repeated URL syntax; PNG bytes equal the corpus UB marker. Repeated bounded offsets match their explicit composed offset. Separate blur references retain intermediate clipping and sampling, so they need not equal two blur primitives in one graph.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| `two-urls`                                   | Compares by default. The boundary difference was Donner's: the three box passes that approximate the Gaussian discarded what the earlier passes spread past the filter raster edge, which the later passes must read back. TinySkia now runs the passes on zero-padded lines, and Geode evaluates the three passes as one cascade kernel per axis that samples the input directly, so a blur near the raster edge equals the same blur computed with room around it.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| `one-invalid-url-in-list`                    | An invalid URL disables the entire chain rather than preserving the prior stage. `FilterReferenceStages.InvalidReferenceDisablesEntireChain` independently covers that behavior; the row compares against a Donner golden.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |

[Filter Effects](https://www.w3.org/TR/filter-effects-1/#FilterFunctions) defines function grammar,
sRGB processing and chaining. Drop-shadow now rejects nonzero unitless CSS lengths while preserving
SVG presentation-attribute user units and unitless zero. Negative standard deviations reject the
whole declaration. Its color alpha is applied once; flood-opacity is a separate factor. Focused
regressions first reproduce invalid declarations changing output and translucent shadows receiving
alpha twice, then pass on TinySkia, full text and Geode. The full property suite also checks that
rejected declarations preserve the prior value at the same cascade priority.

The named gates are `properties_tests`, `renderer_driver_tests`, the three
`renderer_regression_tests` configurations and all three resvg corpus targets. A raster sanitizer
seed covers valid translucent shadows and invalid declarations. The qualified corpus cases name
their cause: label-glyph differences under #1167, and drop-shadow references whose offsets are
truncated, which compare against Donner goldens. #1169 tracks these classifications.
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
the rows compare against Donner goldens backed by visible-geometry assertions, documented
in [0009](0009-resvg_test_suite_bugs.md#invalid-filter-url-reference-qualifications).

## External resource policy

The resvg harness uses a test-only loader that maps the two exact HTTPS identifiers in
`structure/image/url-to-png.svg` and `url-to-svg.svg` to committed local copies. Other URL schemes
are rejected, and ordinary local references still use `SandboxedFileResourceLoader`. Snapshot
reads are bounded to 512 KiB; attribution and source hashes are recorded beside the files. Tests
perform no live network requests. Applications independently provide resource bytes through
`ResourceLoaderInterface`; deterministic image-loader tests cover accepted PNG/SVG bytes, denied
requests and resource-size limits.

The local `structure/use/xlink-to-an-external-file.svg` resource resolves and renders its text
in the `Noto Sans` it inherits from the `use`, with glyphs within 0.4 device px of the reference.
That reference is entirely opaque, whereas the SVG canvas is transparent outside painted content,
and its glyph stems are 0.6 device px wider; resvg and the browsers fail this row in the corpus
results, so it compares against a Donner golden on the transparent canvas, a harness mismatch.
An independently authored inline-versus-external
text-fragment comparison covers host style inheritance, fragment selection, the callback
identifier and transparent canvas behavior.

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
independent axis resolution, transformed/nested clips and invalid declaration fallback. A single
`closest-side` or `farthest-side` keyword selects both radii. Corner extent keywords and `calc()`
radii are not supported; explicit radii use the two-value form.

`clip-path: polygon()` supports comma-separated length/percentage vertices, optional `nonzero`
or `evenodd` interior rules, and the same SVG reference boxes. Vertices may lie outside the
reference box. Valid one-vertex, two-vertex and collinear polygons create empty clips; invalid
syntax preserves the earlier declaration. Vertex storage shares the SVG point-list limit and
render preparation admits all polygon work before constructing paths. Retained vertex capacity is
charged for each computed style, including explicitly inherited copies. Polygon corner rounding
and `calc()` coordinates are unsupported. `polygon_clip_path_tests`, `properties_tests` and
`shape_system_tests` cover rendering, nested rules, grammar and resource refusal.

`clip-path: inset()` supports one to four offsets with margin-style expansion, negative offsets,
and the same SVG reference boxes. Accepted units are absolute lengths, `em`, `rem`, and percentages.
Lengths compute to pixels using the declaring element's computed font size and actual root font
size before explicit inheritance. Horizontal percentages use reference width; vertical percentages
use reference height. The font-metric units `ex`/`ch` and viewport units `vw`/`vh`/`vmin`/`vmax` are
rejected until their computed-value context is available. Unrepresentable computed lengths produce
an empty clip. Oversized opposing insets reduce to zero area, which
produces an empty clipping region rather than preserving an earlier declaration. Optional `round`
radii accept one to four horizontal values and an optional slash with one to four vertical values,
using the same supported units. Radius percentages use the original reference box dimensions;
all corners share one overlap reduction constrained by the used inset rectangle. Negative or
nonfinite radii invalidate the declaration, and a zero radius on either axis makes a square corner.
Fixed inline radius arrays add no dynamic radius storage; rounded path construction admits its
maximum command and point work before allocating geometry. `calc()` remains unsupported.
`inset_clip_path_tests`, `properties_tests` and `shape_system_tests` cover exact rendering, nested
clips, grammar, inheritance, mutation, numeric limits and work refusal. See the
[CSS Shapes inset definition](https://drafts.csswg.org/css-shapes-1/#funcdef-inset).

`clip-path: path()` supports quoted SVG path data with an optional `nonzero` or `evenodd` rule,
using the existing SVG path parser and the same reference boxes. Coordinates are pixel offsets
from the reference-box origin; selecting a box translates the path without scaling it. Every open
contour closes for clipping. Malformed or empty data rejects the entire declaration; valid
zero-area paths produce empty clips. The function's interior rule takes precedence over SVG
`clip-rule`. Decoded strings are limited to 64 KiB before parsing, command and point expansion
uses the SVG parser's limits, and computed styles charge retained path-vector capacities including
inherited copies. Render preparation reserves validation, copy and contour-closing work before
allocation and rejects unrepresentable translated coordinates or extents. `path_clip_path_tests`,
`properties_tests`, `style_system_tests` and `shape_system_tests` cover these contracts.
Path data accepts SVG numeric coordinates, not CSS units, percentages or `calc()`.
See [CSS Shapes path()](https://drafts.csswg.org/css-shapes-1/#funcdef-basic-shape-path) and
[CSS Masking reference boxes](https://drafts.csswg.org/css-masking-1/#the-clip-path).

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
compares at default settings in both CPU text tiers, as do
`text/text/xml-lang=ja`, `text/letter-spacing/non-ASCII-character` and
`structure/defs/style-inheritance-on-text`. Face
selection for these cases is asserted directly in `FontSelection_tests.cc`.

- **Selection and loading:** the generic families map to the faces upstream's
  harness uses (Noto Serif, Noto Sans, Noto Mono, Yellowtail, Sedgwick Ave
  Display). The simple tier now reads GPOS pair kerning wrapped in Extension
  lookups, which Source Sans Pro uses exclusively; `font-list` and
  `source-sans-pro` lost their 1300-pixel simple-tier budgets.
- **Upstream references:** the generic-family PNGs, `xml-lang=ja.png`, `fallback-1.png` and
  `letter-spacing/non-ASCII-character` are upstream resvg's renderings, byte-identical to the
  resvg v0.50.0 release (provenance in `third_party/resvg-test-suite/BUILD.bazel`); they draw in
  the vendored faces, and `non-ASCII-character` quotes its family. `xml-lang=ja` draws all three
  lines in Mplus 1p, the only vendored face covering them, so it does not exercise
  language-specific face selection.
- **Weights 400 and 700 only:** the comparison fixture registers only these
  weights. The snapshot's `font-weight` references draw weights 100 to 300 with
  Noto Sans Regular and 900 with Noto Sans Bold, and Donner matches them with zero
  differing pixels. resvg v0.49.0 draws 900 with Noto Sans Black, so registering
  the other weights belongs with a refresh of those references.
- **Color-adjust labels:** the 8-unit Noto Sans Regular labels have identical geometry across
  the seven cases. The vendored PNGs reproduce macOS font smoothing and draw more ink at the
  glyph edges. The [qualified references](#color-adjust-label-references) keep the upstream
  filter pixels and compare all seven live-text inputs at default settings.
- **`font` attribute:** `text/font/simple-case` disagrees on the `font` shorthand, which SVG 2
  does not list as a presentation attribute; it compares against a Donner golden (see the
  reference qualification table).
- **Default family:** CSS leaves the face for a family list that matches nothing
  to the user agent. resvg appends its default family, serif, to every list, so
  the comparison harness sets serif as `FontManager`'s default family and
  `fallback-1` draws Noto Serif like the reference; the product default remains
  the embedded Public Sans.

> The remaining sub-1000-px CPU thresholds (feColorMatrix saturate and non-normalized values,
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
- The detection a deliberate regression gets is measured and recorded.

No registered case carries such a budget. `text/baseline-shift/nested-with-baseline-1` and `-2`,
`text/dominant-baseline/hanging` and `text/textLength/zero` compare at default settings against
upstream resvg's 300x300 references, which cover the 0.75-pixel crosshair with alpha 95 and 96 on
its two columns, the analytic coverage of 0.375 each.

The two mixed-font ligature fixtures compare against upstream resvg's references, byte-identical
to the resvg v0.50.0 release, at the default settings on the full text tier. These fixtures
exercise the Amiri font's GSUB substitution, which the simple tier does not implement.


`//tools/resvg_parity:parity_report_tests` checks the form of these reference-generation budgets. Such a
compared case:

- comes from a per-case override, not a category default;
- has a pixel budget with no other exception (no simple-text, Geode or golden override);
- keeps the default color threshold;
- has a budget at most its stated analytic residual plus 3%, rounded up to 5 pixels;
- states its supersampled residual, which for this class must be within the default allowance.

The parity report counts these cases as `reference_generation_budget_cases`.

### Color-adjust label references

Switching only font smoothing in Chromium reduces the difference against each original 500 × 500
reference from 461 pixels to 7. Firefox reproduces the effect over white (450 to 12). The
filtered squares agree independently. An upstream resvg 0.48.1 render at the same size still
differs by more than 100 pixels on the 0% case. Even identical glyph paths differ at their
edges when rasterized by different backends.

The [golden README](../../donner/svg/renderer/testdata/golden/README.md#color-adjust-filter-references)
records the independent outline construction, the per-backend rasterization and the comparison
controls. Only the label bands are replaced. All seven rows use the ordinary comparison settings.

### Removed budgets

Budgets that already passed at default settings were removed from 39 cases, including the
`structure/transform-origin` category default (0.13 threshold, 300 pixels), `text/text/xml-space`
(1400 pixels) and `painting/fill/pattern-on-text` (2100 pixels). Measured at default settings, their
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

In upstream resvg's renderings every raster-image reference is the Mitchell cubic (nearest for
`optimizeSpeed`). Donner now resamples `<image>` with `image-rendering: auto`, `optimizeQuality`
and `high-quality` through the same cubic on TinySkia (tiny-skia `FilterQuality::Bicubic`) and
Geode (a 16-tap cubic image-blit sampling mode), matching the kernel `feImage` already used;
`smooth` stays bilinear and compositor bitmap blits keep bilinear sampling. resvg renders an
`feImage` raster as an image, so `feImage` placement shares the same `ImageSamplingKernelFor`
mapping on both backends: `smooth` is bilinear there too, and a pure translation samples the
containing texel, as tiny-skia does for `<image>`. That also removed a cubic filtering pass that
rotated `feImage` placement and nested-SVG `feImage` rasters received at one-to-one scale, which
re-enabled `filters/feImage/with-subregion-5` and lets `filters/feImage/svg` compare directly
against a 500x500 resvg v0.49.0 reference. The raster-image references are upstream resvg's
renderings, byte-identical to the resvg v0.50.0 release, with provenance in
`third_party/resvg-test-suite/BUILD.bazel`.

The `embedded-svg-with-text` row is a harness mismatch and compares against a Donner rendering
(`testdata/golden/resvg-embedded-svg-with-text.png`) at the default settings on every backend. The
vendored reference omits the embedded SVG's text, which browsers and resvg draw; the resvg v0.49.0
rendering draws it but quantizes the embedded frame lines' coverage to quarters. The golden encodes
a harness-only configuration: the suite gives SVG image documents its test fonts, as a stand-in for
the system fonts an image document sees in a browser or in resvg's process-wide font database. The
product renders an image document's text with only the built-in font catalog. An authored
`EmbeddedSvgUsesDocumentFontContext` case checks that product behavior (embedded-versus-direct
identity plus visible text ink) in `//donner/svg/renderer/tests:renderer_public_api_tests` and its
`//donner/svg/renderer/tests:renderer_public_api_tests_geode` variant.

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
`lengthAdjust/vertical` compares against upstream resvg's 300x300 rendering. The resvg-test-suite
snapshot's reference places the sideways glyphs 0.28 units across the line from both Donner and
resvg, which is consistent with a rendering from before the central baseline. Decorations follow
the adjusted glyph extents; `lengthAdjust/with-underline` and
`text-decoration/with-textLength-on-a-single-character` also compare against upstream resvg's
references, because the snapshot's draw their underlines at 0.1em, twice the 0.05em thickness in
the vendored Noto Sans post table.

`textLength/zero` compares at default settings against upstream resvg's 300x300 reference, which
Donner matches within 1 pixel on both CPU text tiers. SVG 2 applies anchoring from the compressed
pen extents after textLength adjustment, which moves this fixture's start-anchored glyphs by the
final glyph advance (17.328 units); the resvg-test-suite snapshot's reference places them about
17 units further left.

### F10: `textPath` SVG2 attributes

Inline `path`, geometry references, `side=right`, `method=stretch`, and `spacing=auto` are
implemented. Their focused geometry and renderer contracts are active. The vendored `method=stretch`
image is identical to the original snapshot’s align reference, so that corpus row compares against a Donner-rendered
reference (see the [golden README](../../donner/svg/renderer/testdata/golden/README.md)) while
independent curved-outline tests distinguish the methods. A fragment-free relative `xlink:href`
under the secure static resource policy does not resolve to a same-document element or initiate an
external fetch; its conflicting corpus image is skipped. The
`TextPathInvalidInlineAndFragmentFreeHrefPaintsBlank` case enforces that policy in
`//donner/svg/renderer/tests:renderer_public_api_tests` and
`//donner/svg/renderer/tests:renderer_public_api_tests_geode` using a visible control scene and a
resource loader that records requests.

`closed-path.svg` compares its CPU lanes against a Donner-rendered reference and its Geode lane
against a copy of the resvg v0.50.0 reference: the vendored image predates resvg's arc-length
placement, and the release reference inverts arc length to within 0.5 user units, which leaves
its glyphs up to 0.82 px from Donner's placement (see the
[golden README](../../donner/svg/renderer/testdata/golden/README.md)).

`mixed-children-2.svg` compares against a Donner-rendered reference: SVG 2 continues the text
after a `textPath` from the path's end point, while the vendored image continues from the end of
the glyphs on the path (see the [golden README](../../donner/svg/renderer/testdata/golden/README.md)).

Vertical layout on a path (`text/textPath/writing-mode=tb.svg`) compares at default settings
against a Donner golden: the reference moves upright glyphs off the font's vmtx origin, which CSS
Writing Modes 3 section 5.1.1 requires.
`complex.svg` is undefined behavior: its reference is the resvg UB marker image.
`textPath/with-filter.svg` and `tspan/with-filter.svg` compare at default settings against Donner
goldens: their references draw glyph edges 0.25 to 0.3 px outside the outline, which the blur
spreads into the glyph interiors, while Donner's glyph ink equals the outline's area
(`GlyphInkAreaTest`; see the [golden README](../../donner/svg/renderer/testdata/golden/README.md)). Where the reference does not use the font's vertical metrics
and features, which CSS Writing Modes 3 section 5.1.1 requires (`writing-mode/japanese-with-tb.svg`,
`writing-mode/mixed-languages-with-tb.svg`, `writing-mode/tb-and-punctuation.svg`,
`alignment-baseline/hanging-on-vertical.svg`), the row compares against a Donner rendering
([0009](0009-resvg_test_suite_bugs.md)). `writing-mode/mixed-languages-with-tb-and-underline.svg`
renders without a comparison: CSS Text Decoration 3 leaves the underline's position and
thickness to the user agent.

### F11: BiDi / RTL text shaping

**Impact:** ~8 across `text/direction` (2), `text/unicode-bidi` (1),
`text/text/bidi-reordering`, `text/tspan/bidi-reordering`,
`text/letter-spacing/mixed-scripts`, `text/textLength` Arabic. Needs the BiDi
algorithm + RTL shaping (`text-full`). Group as one workstream.

**Current status:** normal horizontal BiDi, `direction`, and a `bidi-override` on the `<text>`
element itself are implemented ([0069](0069-bidi_text_layout.md)). `text/unicode-bidi/bidi-override`
compares on the full text tier at default parameters, with 100 differing pixels at the default
limit of 100. `text/direction/rtl` compares on the full text tier at default parameters against
the vendored Noto Sans Arabic face its reference uses.
`text/direction/rtl-with-vertical-writing-mode` is undefined behavior (svgwg#618).

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
| structure/style           |     0 | `external-CSS` compares after CSS `@import` loading landed: imports load through the document resource loader with cycle, depth, count and byte bounds; `//donner/svg/renderer/tests:css_import_tests` covers fetches, relative resolution, limits and cascade order.                                                                                |
| structure/symbol          |     1 | `transform` on `<symbol>` (SVG2)                                                                                                                                                                                                                                                                                                                     |
| painting/image-rendering  |     0 | Both backends implement the typed values, including two-stage non-integer `pixelated` scaling. Both TinySkia and Geode compare `optimizeSpeed` and `on-feImage` against resvg v0.50.0; the latter samples the embedded image at destination pixel centers. |
| masking/clipPath          |     0 | CSS circle shapes support fill, stroke and viewport reference boxes with bounded geometry preparation; two opaque reference images retain explicit background qualifications. The transformed-text case compares against resvg v0.49.0 with zero differing pixels; both use the glyph-cell text box.                                                 |
| masking/mask              |     0 | Transformed group and rotated mask regions compare with ordinary budgets; `mask-type` alpha/luminance and mask `color-interpolation` sRGB/linearRGB are active.                                                                                                                                                                                      |
| text/font                 |     2 | `font` shorthand; canvas-size mismatch (test harness)                                                                                                                                                                                                                                                                                                |
| text/tspan                |     0 | tspan `clip-path`, `filter` and `mask` compare; `with-filter` compares against a Donner golden because its reference draws glyph edges outside the outline                                                                                                                                                                                           |
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
| text/writing-mode | ~7 | `vertical-lr` / `vertical-rl` edge cases and mixed-script (upright CJK plus rotated Latin) column geometry. Obsolete `tb*` SVG 1.1 values should be classified or mapped to their required compatibility aliases, not implemented as distinct modes. Font fallback is per cluster, so the `mixed-languages-with-tb*` cases draw their Arabic words; vertical anchoring measures chunks between pen positions. `mixed-languages-with-tb` compares against a Donner rendering because its reference sets upright CJK lower than the font's vmtx origin. The underline case renders without a comparison: CSS Text Decoration 3 leaves underline position and thickness to the user agent. `tb-and-punctuation` compares against a Donner rendering. |

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
marker. `radialGradient/fr=0.7` compares against its vendored reference at default settings,
and `svg/no-size` and `font/simple-case` compare against Donner goldens; no comparison tolerance is
widened.

| Assigned fixture                               | Supported behavior and reference disposition                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| ---------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `radialGradient/fr=-1`                         | Negative focal radius is invalid and applies no paint. UB marker reference.                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| `radialGradient/fr=0.5`                        | Coincident start/end circles apply no paint today; whether they should is an open question (see below). UB marker reference.                                                                                                                                                                                                                                                                                                                                                                                             |
| `radialGradient/fr=0.7`                        | A start circle containing the end circle paints the whole plane, by the canvas two-point rule that SVG 2 `fr` follows. Compares against the vendored reference at default settings.                                                                                                                                                                                                                                                                                                                                       |
| `radialGradient/invalid-gradientTransform`     | The authored all-zero matrix is singular, rather than malformed syntax. No paint; UB marker reference.                                                                                                                                                                                                                                                                                                                                                                                                                    |
| `radialGradient/invalid-gradientUnits`         | The invalid enumeration is diagnosed and the initial `objectBoundingBox` value is retained. UB marker reference.                                                                                                                                                                                                                                                                                                                                                                                                          |
| `radialGradient/negative-r`                    | Negative outer radius is invalid and applies no paint. UB marker reference.                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| `fill/rgb-int-int-int`                         | Space-separated RGB is supported CSS Color 4 syntax and matches the equivalent hex color. UB marker reference.                                                                                                                                                                                                                                                                                                                                                                                                            |
| `fill/valid-FuncIRI-with-a-fallback-ICC-color` | The obsolete SVG 1.1 `icc-color()` paint grammar is unsupported. Invalid CSS declarations are rejected, preserving inherited paint; the historical reference expects a gradient. `Params::DeprecatedInSVG2` (removed in SVG 2): rendered, not compared.                                                                                                                                                                                                                                                                   |
| `stroke-linejoin/arcs`                         | The SVG Strokes draft join is not implemented; Donner retains its documented miter fallback. The reference is the UB marker, not an arcs rendering.                                                                                                                                                                                                                                                                                                                                                                       |
| `stroke-linejoin/miter-clip`                   | Separate Geode/editor parity repair and ordinary reference restoration: [#2057](https://github.com/jwmcglynn/donner/pull/2057).                                                                                                                                                                                                                                                                                                                                                                                           |
| `svg/no-size`                                  | An unsized root has no intrinsic size, and SVG 2 leaves its initial viewport to the host; Donner's API gives it the caller canvas (512x512 by default) at one user unit per px. The reference shows the 200-unit artwork in a 200-unit viewport at 2.5x. API policy: compares against a Donner golden; the caller-canvas contract has an independent assertion.                                                                                                                                                                                                                                                                                          |
| `font/simple-case`                             | The PNG and its golden are 400x400, the canvas the harness takes from the golden. SVG 2 lists no `font` presentation attribute, so the `font` attribute is ignored and the case draws the initial 12px serif text. The vendored PNG applies the attribute as SVG 1.1 did and synthesizes bold over Noto Sans Italic. Compares against a Donner golden (#1167).                                                                                                                                                                                |

[SVG 2 radial-gradient rules](https://www.w3.org/TR/SVG2/pservers.html#RadialGradients) and [error
processing](https://www.w3.org/TR/SVG2/conform.html#ErrorProcessing) establish the radial error
expectations. The [canvas two-point
rule](https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createradialgradient) that
`fr` follows paints a start circle that contains the end circle. Identical circles are an open
question: the SVG 2 note and the canvas rule paint nothing, which Donner does today, while [svgwg
issue 648](https://github.com/w3c/svgwg/issues/648) and the WPT test
`svg/pservers/reftests/radialgradient-fully-overlapping.svg` expect the gradient drawn, so `fr=0.5`
stays render-only and a regression test pins the current behavior. The independent tests preserve
visible sibling artwork and a valid zero-radius last-stop control. Both backends reject negative
radii before solid-color shortcuts, including single-stop gradients and an outer radius within the
CPU near-zero tolerance. [CSS Color 4](https://www.w3.org/TR/css-color-4/#rgb-functions) defines the
RGB syntax; [SVG 2 paint grammar](https://www.w3.org/TR/SVG2/painting.html#SpecifyingPaint) does not
include the old `icc-color()` suffix. The `arcs`, unitless font shorthand, caller canvas and
zero-radius controls are explicitly project compatibility contracts, not claims of broader SVG 2
conformance.

The focused regression targets are `renderer_regression_tests`, `renderer_regression_tests_geode`
and `renderer_regression_tests_text_full`. The corpus targets are `resvg_test_suite_default_text`,
`resvg_test_suite_max`, `resvg_test_suite_geode` and `resvg_test_suite_geode_max_text`. The Geode
lanes compare the text rows in both text tiers; the independent Geode font contract test executes
directly. Render-only corpus execution of the UB-marker cases establishes stability, not pixel
agreement with the historical reference.

## Out of scope (correctly not compared; do not "fix")

| Category or case                                                          | Tests | State and why                                                                                                                                                                                                                                                                     |
| ------------------------------------------------------------------------- | ----: | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| filters/enable-background                                                 |    17 | Category default `Params::DeprecatedInSVG2(...)`: removed in SVG 2 (→ `<filter>` chains / `backdrop-filter`). The other four of the 21 cases already match the reference with SVG 2 semantics and compare. See [`unsupported_svg1_features.md`](../unsupported_svg1_features.md). |
| filters/filter `in=Background*-with-enable-background`                    |     2 | `Params::DeprecatedInSVG2`: the BackgroundImage/BackgroundAlpha snapshot comes from `enable-background`.                                                                                                                                                                          |
| text/tref, painting/display/none-on-tref                                  | 9 + 1 | `Params::DeprecatedInSVG2`: `<tref>` removed in SVG 2.                                                                                                                                                                                                                            |
| text/kerning                                                              |     2 | `Params::DeprecatedInSVG2`: `kerning` property removed in SVG 2.                                                                                                                                                                                                                  |
| text/glyph-orientation-horizontal                                         |     1 | `Params::DeprecatedInSVG2`: removed in SVG 2. `glyph-orientation-vertical/simple-case` is user-agent defined and renders only (see below).                                                                                                                                                              |
| painting/fill `icc-color`, `valid-FuncIRI-with-a-fallback-ICC-color`      |     2 | `Params::DeprecatedInSVG2`: SVG 1.1 `icc-color()` paint is not in the SVG 2 paint grammar.                                                                                                                                                                                        |
| structure/style-attribute, structure/style `non-presentational-attribute` |     2 | `Params::DeprecatedInSVG2`: `<svg version="1.1">` semantics; SVG 2 removed the `version` attribute.                                                                                                                                                                               |
| masking/clip                                                              |     1 | `Params::DeprecatedInSVG2`: the CSS `clip` property is deprecated by CSS Masking in favor of `clip-path`.                                                                                                                                                                         |
| text/dominant-baseline `reset-size`, `use-script`                         |     2 | `Params::DeprecatedInSVG2`: SVG 1.1 values removed in SVG 2; UB marker references.                                                                                                                                                                                                |
| Other `Params::UndefinedBehavior` cases                                   |    55 | Undefined or implementation-defined behavior: 52 on the resvg UB marker reference plus `feGaussianBlur/huge-stdDeviation`, `glyph-orientation-vertical/simple-case` and `writing-mode/mixed-languages-with-tb-and-underline`; we verify no-crash only.                                                                                                                               |

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
  attach an exact local `withGeodeGoldenOverride(...)` and current reason to that case. Never widen
  a Geode-only threshold to absorb the difference. Disabling the Geode comparison of a row is
  limited to `GEODE_GATE_ALLOWLIST` in `parity_report.py`: each entry records a measured cause.
  `//tools/resvg_parity:parity_report_tests` enforces in CI that the list equals the Geode-gated
  rows; that it only shrinks is a review rule.
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
- `filters/feColorMatrix/type=matrix-with-non-normalized-values` uses a Geode-specific golden.
- `filters/feImage/svg` compares on both backends at default settings against a 500x500 resvg
  v0.49.0 rendering. Explicitly positioning the nested SVG in the filter region produces the
  same reference.

**Lesson:** a large diff amplified by a filter/matrix is not evidence of a coverage
problem — inspect whether a coverage change actually moves it before attributing it.
The registry has no explicit per-fixture backend disable. The Geode corpus still
applies its feature gate for text. Verified analytic-vs-finite-sample edge
residuals, including `feGaussianBlur/complex-transform`, use exact Geode goldens. Paint-order `on-text` and the rotated pattern case
(`transform-and-patternTransform`) compare on Geode against their vendored references at default
settings (42 and 55 pixels on Metal and on Linux lavapipe).

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

## Text decoration thickness

`text-decoration-thickness` accepts `auto`, `from-font`, absolute lengths, `em`, `rem`, and
percentages. The font-metric units `ex`/`ch` and viewport units `vw`/`vh`/`vmin`/`vmax` are rejected
until their computed-value context is available in the style pipeline. Explicit
percentages use the originating decoration's used font size; font-relative lengths compute before
explicit inheritance. The supported `text-decoration` shorthand resets omitted thickness to `auto`
and preserves independent cascade priorities. Draft line-width keywords, underline offsets,
shadows, and emphasis remain unsupported.

Each originating declaration retains its own font, paint and thickness, including repeated
underline declarations on nested spans. The software and GPU renderers consume the same bounded
decoration geometry. `from-font` uses the preferred underline width for all line kinds and falls
back to automatic metrics when unavailable. Automatic placement retains the existing behavior.

Immutable decoration lists are limited to 64 originating declarations per span and charged to the
shared geometry budget. Snapshot copies retain and remap their own font and paint resources; a
refused list suppresses those decorations. New live-bitmap comparisons in
`text_decoration_thickness_tests` use synthetic font metrics and the canonical exact comparator.

## Text decoration painting order

Underlines and overlines paint beneath a run's glyphs and line-throughs above them, following the
Painting Order of Text Decorations in CSS Text Decoration 3, which SVG 2 section 11.11 adopts.
Each decoration's own fill and stroke follow the `paint-order` of the element whose paint it uses.
Both renderers make two decoration passes per run around the glyph paint, and
`text_decoration_paint_order_tests` compares overlapping glyph and decoration colors exactly. With
per-origin paint and this order, `text/text-decoration/indirect-with-multiple-colors` compares
against the independent full decoration scene at the ordinary threshold. At 800 pixels, its
diagnostic crosshair spans two whole pixels.

## Text decoration line, style and color

`text-decoration-line`, `text-decoration-style` and `text-decoration-color` are CSS properties; the
`text-decoration` shorthand sets all four longhands and resets omitted components. All four are
not inherited. SVG 2 section 6.6 lists only `text-decoration` as a presentation attribute, so the
longhands are accepted in style sheets and `style` attributes rather than as attributes.

`text-decoration-style` draws `solid`, `double`, `dotted`, `dashed` and `wavy` lines from the
solid band of thickness `t` (CSS Text Decoration 3 leaves the proportions to the user agent):
`double` adds a second band after a gap of `t`, away from the text (a line-through straddles its
solid position); `dotted` draws round dots of diameter `t` every `2t`; `dashed` draws dashes `3t`
long every `5t`; `wavy` draws a band of thickness `t` along quadratic half-waves of amplitude `t`
whose length divides the segment into the whole count nearest `2t`. Styled geometry is charged
point by point to the per-draw decoration work allowance, which each text draw takes from a
per-frame allowance of 4 Mi work units on the text materialization budget. A line that would
exceed the per-draw allowance is not drawn and spends the rest of it, so the exhausted allowance
also suppresses later decorations of that draw and, once the frame allowance is spent, of the
frame. The frame's text itself is not rejected.

SVG 2 section 11.11 paints a decoration with the fill and stroke of the element that declares it
and does not define `text-decoration-color`. Its initial value in CSS Text Decoration 3 is
`currentcolor`, which would replace that fill for every SVG decoration, so Donner treats the
unset longhand as "use the declaring fill" and an author-specified color, including
`currentcolor`, as the decoration's fill color. The decoration keeps the declaring element's
stroke and fill opacity, and `currentcolor` resolves against the declaring element's `color`.
`text_decoration_style_tests` compares each style and color rule exactly against authored scenes.
