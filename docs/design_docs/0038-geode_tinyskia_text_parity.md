# 0038 — Geode ↔ tiny-skia text parity: developer reference

**Status:** Developer reference. Text parity between the Geode and tiny-skia
backends is **complete** — 0 structural divergences remain; the only residual
geode↔tiny diff is the accepted sub-pixel coverage floor ([0041](0041-geode_analytical_aa.md)).
This doc describes the shared text layer both backends consume and how per-test
parity is expressed in `ImageComparisonParams`. Geode runs the same params as the CPU
variants; a parity-only exception is a per-test `disableGeodeParity(reason)` (see
[0021 §Geode / Resvg Override Policy](0021-resvg_feature_gaps.md#geode--resvg-override-policy)).
The §4 catalog records the resolved divergences as implementation notes.

**Related:** [0017 §Phase 4b](0017-geode_renderer.md#phase-4b-in-process-backend-matrix--geode-vs-tiny-skia-parity-comparison),
[0041 anti-aliasing](0041-geode_analytical_aa.md),
[0042 Slug implementation](0042-geode_slug_conformance.md),
[0021 §Geode / Resvg Override Policy](0021-resvg_feature_gaps.md#geode--resvg-override-policy)

---

## 1. How text flows through both backends

`RendererGeode::drawText` and `RendererTinySkia::drawText` are two backend-specific
consumers of one **shared text layer**. Everything that determines *what* to draw —
glyph placement, paint resolution, decoration geometry, font-size/scale — is computed
once, above both backends and below `TextEngine`; each backend only rasterizes the
result.

### 1.1 Shared layout (above both backends)

- **`ComputedTextGeometryComponent` (`runs` cache) + `toTextLayoutParams`** produce
  per-glyph `{xPosition, yPosition, rotateDegrees, stretchScale, fontSizeScale}`.
  These values are **identical between backends** — neither backend re-derives glyph
  positions. A backend can only diverge in how it *consumes* these values.
- **`spanFontSizePx` + `scaleForPixelHeight`** (per-element and per-span font-size
  resolution, including named keywords `xx-small`…, percent, and negative sizes) are
  computed by a byte-identical expression in both backends from the same inputs.
- **`resolvePerSpanLayoutStyles` (TextEngine)** resolves nested baseline-shift. It
  clears `span.ancestorBaselineShifts` before re-populating, so repeated `draw()`
  calls on the same `ComputedTextComponent` are idempotent (see B1–B6 in §4).

### 1.2 Shared placement + bounds: `PlacedTextGeometry`

`donner/svg/renderer/PlacedTextGeometry.{h,cc}` is the pure-geometry layer both
backends call (lib target, no backend/paint types):

- **`placedGlyphOutline(textEngine, font, glyph, …)`** (`PlacedTextGeometry.h:60`)
  returns the placed outline `Path` for one glyph, encoding the canonical sequence:
  `glyphOutline(font, idx, scale * fontSizeScale)` → stretch on the **raw outline**
  (`Scale(stretchX, stretchY)`) → `Rotate(rotateDegrees) * Translate(x, y)`. Returns
  empty for outline-less glyphs (bitmap-only fonts, `.notdef`), which each backend
  skips. Both `drawText` paths call this instead of re-deriving the placement
  transform.
- **`transformPath(path, transform)`** (`PlacedTextGeometry.h:39`) — one shared
  definition, replacing the previously-duplicated byte-identical free function in
  each backend.
- **`computeTextBounds(textEngine, runs, …)`** (`PlacedTextGeometry.h:83`) — the
  text bounding box used to resolve `objectBoundingBox` gradients/patterns on text.
  One implementation serves both backends.

### 1.3 Per-backend consumers (`drawText`)

Each backend's `drawText` loops the shared placement/bounds output and rasterizes:

- **Geode** (`RendererGeode::drawText`): routes glyph outlines through the Slug fill
  pipeline; gradient fill/stroke through `drawPaintedPathAgainst(textBbox, …)`;
  pattern fill/stroke through the `patternFillPaint` / `patternStrokePaint` slots;
  decorations (underline/overline/line-through) as filled/stroked rects.
- **tiny-skia** (`RendererTinySkia::drawText`): the same logical steps on the
  tiny-skia rasterizer.

**Invariant:** tiny-skia text output is the parity reference. Any change to the
shared layer must keep tiny-skia byte-identical (verified against
`:resvg_test_suite` text tests + `:renderer_tests`); Geode converges to it.

---

## 2. Parity status

**0 structural text divergences** (catalog in §4). The remaining geode↔tiny text diff is
the accepted-by-design sub-pixel coverage floor — geode renders the correct
glyphs/positions/colors; the residual is the thin edge band + the resvg harness 0.5px
crosshair overlay (see [0041 §2](0041-geode_analytical_aa.md), proven sample-independent).
No text test needs a parity exception: the residual stays within each test's normal
`ImageComparisonParams` budget.

---

## 3. How parity is expressed (for text)

Parity runs in the **geode-enabled build** of `//donner/svg/renderer/tests:resvg_test_suite`
(it rides the `*_geode` wrapper under `bazel test //...`). Each test runs up to three
comparison modes; text is validated on the `GeodeTinyParity` mode. See
[0017 §Phase 4b](0017-geode_renderer.md#phase-4b-in-process-backend-matrix--geode-vs-tiny-skia-parity-comparison)
for the full mode matrix.

**Policy (text and non-text alike):** `GeodeTinyParity` compares geode↔tiny-skia at
each test's **own** `ImageComparisonParams` threshold and max-pixel budget — the same
budget its golden comparison uses — with pixelmatch `includeAA=false`. There is no
separate geode threshold table; a parity diff over the test's budget fails, never
absorbed by a larger budget (that would be masking). See
[0021 §Geode / Resvg Override Policy](0021-resvg_feature_gaps.md#geode--resvg-override-policy).

**To add a text test:** add it to the suite as usual. If it renders correctly but its
geode↔tiny diff exceeds the test's budget (the accepted edge floor), attach a
`disableGeodeParity("<reason, e.g. 0039 edge floor>")` to that test's `Params`. If it
renders *wrong*, that's a real bug — fix the shared layer or the backend consumer,
don't add an exception. The standing goal (per 0021) is *fewer* exceptions over time.

> The parity oracle is tiny-skia, so a tiny-skia text regression could mask a geode
> one. This is mitigated because the `TinyGolden` mode gates tiny-skia against the
> resvg ground truth in the same run.

---

## 4. Resolved divergence catalog (implementation notes)

Each entry below was a place the two backends drifted (or where Geode was missing a
feature). All are resolved; they double as regression-relevant implementation notes.

### 4.1 Feature gaps fixed in Geode's `drawText`

| # | Divergence | Root cause | Fix |
|---|---|---|---|
| D1 | text-decoration not drawn | geode drew no underline/overline/line-through | `d1742348c` — decoration geometry |
| D2 | stroked-glyph ring fill rule | geode used `NonZero` (solid interior); the ring needs `EvenOdd` | `2314efb0d` — stroke→fill |
| D3 | pattern-fill on text | geode `drawText` had no pattern path → glyphs unfilled + a staged `patternFillPaint` slot leaked to the next shape | `1e2eb2b6f` — paint resolution |
| D4 | stretch+rotate transform order | tiny applied stretch on the raw outline then `Rotate*Translate`; geode used `Scale*Rotate*Translate` — diverges only when `stretchScale≠1` **and** `rotate≠0` | structurally fixed by `placedGlyphOutline` (latent: no suite test triggers it) |

### 4.2 Paint resolution against the text bbox

Geode `drawText` was **missing** gradient handling and had incomplete pattern
handling — `resolveSpanFill`/`resolveSolidStroke` collapsed gradient refs (glyphs
unfilled / element-gradient text rendered nothing), and only the `patternFillPaint`
slot was consumed, never `patternStrokePaint`. Fix (reusing geode's existing gradient
infra, not a new abstraction): geode computes the text bbox via the shared
`computeTextBounds`, routes gradient fill/stroke through `drawPaintedPathAgainst(textBbox, …)`,
and pattern stroke through the `patternStrokePaint` slot. tiny-skia then dropped its
inline text-bbox loop and adopted the same `computeTextBounds` (proven a pixel no-op —
95-test before/after diff identical). **One bbox implementation now serves both
backends.**

| # | test | geode↔tiny px (before → after) | outcome |
|---|---|---|---|
| B15 | `painting/fill/radial-gradient-on-text` | 14562 → 3 | un-gated |
| B16 | `painting/stroke/pattern-on-text` | 13115 → 39 | un-gated |
| B18 | `painting/fill/linear-gradient-on-text` | 10195 → 1 | un-gated |
| B17 | `painting/stroke/linear-gradient-on-text` | 11917 → 465 | edge-floor |
| B11 | `text/tspan/tspan-bbox-2` | 2929 → 694 | edge-floor |
| B12 | `text/tspan/tspan-bbox-1` | 1803 → 702 | edge-floor |

(B19 `paint-servers/pattern/text-child` is a `<pattern>` *containing* text — already
rendered correctly, edge-floor — distinct from B16 which is a pattern *as the text
fill*.)

### 4.3 Nested baseline-shift (shared-layout idempotency)

B1–B6 (the largest cluster) were **not** a geode rendering gap — they were a
shared-layout state-accumulation bug. `resolvePerSpanLayoutStyles` pushed onto
`span.ancestorBaselineShifts` without ever clearing, and it runs **per `draw()`**.
The parity harness draws each document twice (geode then tiny on the same
`ComputedTextComponent`), so the second backend saw **doubled** ancestor shifts.
Position dump: geode (1st pass) `y=74.4` (correct 2×20%), tiny (2nd pass) `y=61.6`
(3×20%). Fix: `span.ancestorBaselineShifts.clear()` before re-populating → idempotent
layout. tiny single-pass output unchanged (clear is a no-op on the empty default;
verified byte-identical across 96 text tests).

| # | test | geode↔tiny px (before → after) |
|---|---|---|
| B1 | `text/baseline-shift/nested-with-baseline-2` | 19750 → 702 |
| B2 | `text/baseline-shift/nested-with-baseline-1` | 12886 → 702 |
| B3 | `text/baseline-shift/mixed-nested` | 4338 → 690 |
| B4 | `text/baseline-shift/deeply-nested-super` | 4320 → 720 |
| B5 | `text/baseline-shift/nested-super` | 2870 → 677 |
| B6 | `text/baseline-shift/nested-length` | 2438 → 686 |

All six now render correctly at the ~677–720 px edge floor. This same
double-draw idempotency class also surfaced a production feImage-fragment bug
(unrelated to text; see [0017 §Phase 4b](0017-geode_renderer.md#phase-4b-in-process-backend-matrix--geode-vs-tiny-skia-parity-comparison)
and the appendix).

### 4.4 Per-char `dy` / `rotate` lists (render-correct edge floor)

| # | test | geode↔tiny px (before → after) |
|---|---|---|
| B7 | `text/text-decoration/underline-with-dy-list-2` | 4643 → 1177 |
| B8 | `text/text-decoration/underline-with-rotate-list-4` | 4561 → 1145 |

The baseline-shift fix cleared the structural part of B7/B8. The residual is the 4×
fringe on the gray stroke-ring + gradient: the plain-black siblings `dy-list-1`
(699 px) and `rotate-list-3` (686 px) are already accepted edge-floor, proving `dy`
and `rotate` are consumed correctly; glyph interiors are zero-diff and double-draw was
ruled out (tiny-twice = 0 px). Both are accepted edge-floor.

### 4.5 Already-correct, reclassified to edge-floor

These were never structural — they render correctly and the diff is cumulative edge
fringe (many lines / long strings / on-path small text / tiled fields):

| # | test | px |
|---|---|---|
| B9 | `text/text-decoration/tspan-decoration` | 1822 |
| B10 | `text/font-size/named-value` | 3488 (named keywords are on `<rect>`s; the text is all size-12 and renders correct) |
| B13 | `text/textPath/dy-with-tiny-coordinates` | 2219 |
| B14 | `text/letter-spacing/on-Arabic` | 932 |
| B19 | `paint-servers/pattern/text-child` | 1663 |

> Note: at strict-0 the characterization also listed `font-size/negative-size` (5588)
> and `tspan/with-opacity` (1599) as bugs; both drop below the 100-px flat budget at
> 0.02 and are not gated.

---

## 5. Related parity findings (not text)

For the record — these were surfaced by the same parity run and are tracked elsewhere:

- **Filter divergences (G2):** the parity run also surfaced 37 pure-filter geode↔tiny
  divergences. **All resolved** — the common root was geode inconsistently applying
  `color-interpolation-filters` (linearRGB default) plus a handful of genuine
  conformance/CTM bugs; details and the close-out are in the appendix and
  [0021 §Geode / Resvg Override Policy](0021-resvg_feature_gaps.md#geode--resvg-override-policy).
- **Sub-visual premultiply fills (~137):** at strict-0, ~137 non-text tests show a
  whole-fill diff that collapses to <100 px at 0.02 (a uniform sub-perceptual
  premultiplied-alpha / color-space rounding offset). They PASS parity within their
  normal budgets; tracked as one root-cause item in
  [0021 §Geode / Resvg Override Policy](0021-resvg_feature_gaps.md#geode--resvg-override-policy).

---

## Appendix — investigation history & rejected approaches

Condensed from the parity push; preserved so the conclusions aren't re-litigated.

### A.1 The original hoist proposal (partially executed, then descoped)

The opening thesis was that the two `drawText`s were full parallel reimplementations
that would keep drifting, and the durable fix was to hoist **all** of placement /
paint / decoration / font-size into a single shared `PlacedText` builder emitting a
backend-agnostic op list, collapsing each `drawText` to a thin op consumer.

What actually shipped: the **geometry** slices were hoisted (`PlacedTextGeometry`:
`placedGlyphOutline`, `transformPath`, `computeTextBounds` — §1.2). The remaining
divergences turned out **not** to be drift in shared logic but either (a) a feature
*missing* from geode (gradient/stroke-pattern on text — fixed by targeted convergence
reusing geode's existing infra) or (b) a shared-layout idempotency bug
(baseline-shift). Once those were fixed there was **no remaining drift** to justify
the larger paint-descriptor abstraction, so the full op-list hoist was **descoped** —
both backends now map the same paint servers + the same `computeTextBounds`. If future
drift reappears, the op-list builder remains the recorded durable fix.

### A.2 Increment-by-increment findings (why the plan changed shape)

- **Per-run scale + font-size (planned increment):** *skipped — already shared.*
  `spanFontSizePx` and `scaleForPixelHeight` are byte-identical expressions in both
  backends; nothing to extract. B10 (`font-size/named-value`) was never a font-size
  bug — reclassified edge-floor.
- **D4 placement order:** structurally fixed by `placedGlyphOutline` but flips **zero**
  gates — no suite test has simultaneous `stretchScale≠1` *and* `rotateDegrees≠0`, so
  the order fix changes no pixels. Kept because it makes D4 impossible for any future
  stretch+rotate test.
- **The "identical positions" contradiction:** early analysis found `glyph.{x,y,rotate}`
  identical between backends (shared `runs` cache), seemingly contradicting whole-glyph
  baseline-shift offsets. Resolution: the positions *were* identical per-pass — the
  divergence was the double-draw accumulation in shared layout (§4.3), not a geode
  consume-path bug.

### A.3 Filter (G2) close-out — for the record

All 37 filter divergences were resolved, mostly by fixing geode's inconsistent
`color-interpolation-filters` (linearRGB) handling:

- **feComposite + feComponentTransfer** ran in sRGB instead of the linearRGB default
  → 8 of 9 color-math tests to 0 px (also corrected a unit test that encoded the bug).
- **feImage** "placement" was misattributed — the real bug was a shared `RendererDriver`
  idempotency leak (`OffscreenFeImage` shadow-tree instances persisting across renders,
  corrupting any host re-rendering an feImage-fragment doc, e.g. the editor). Fixed +
  red→green `FeImageFragmentRedrawIsIdempotent`.
- **feTurbulence** was spec-exact Perlin noise; the only deviation was a skipped
  linearRGB→sRGB output conversion → all 12 to 0 px. The fix unmasked a real
  feDisplacementMap bug (missing un-premultiply, nearest vs bilinear, skipped
  linearRGB), all fixed.
- **feDiffuseLighting / feMerge / feConvolveMatrix** cleared by the linearRGB sweep
  (+ a convolve wrap fix); **feSpecularLighting** was a genuine spec bug (missing
  `specularExponent` [1,128] clamp + `<1`→transparent).
- **feGaussianBlur/complex-transform** was anisotropic-blur-under-rotation
  (`stdDeviation="12 0"` on `rotate(45)`): geode blurred device-axis-aligned while
  tiny blurs in local space. Fixed by porting tiny's transformed-blur path into
  `RendererGeode::popFilterLayer` (35151 → 140 px, the accepted edge floor) — a real
  fix for all rotated/skewed anisotropic blur.

> **Pattern worth remembering:** geode's filter engine inconsistently applied
> `color-interpolation-filters`. The original gap spanned feComposite,
> feComponentTransfer, feTurbulence, feDisplacementMap (all fixed); feGaussianBlur /
> feColorMatrix / feBlend already had it.
