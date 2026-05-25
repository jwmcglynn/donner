# 0038 — Geode ↔ tiny-skia text parity: divergence catalog + hoist-shared-drawText proposal

**Status:** living catalog (opened 2026-05-24). Accumulates every geode↔tiny-skia text
divergence found during the [Phase 4b](0017-geode_renderer.md#phase-4b-in-process-backend-matrix--geode-vs-tiny-skia-parity-comparison)
parity push, then proposes the structural fix. The backend matrix + whole-suite parity
landed green (flat-100 policy); the catalog below is the complete set of text divergences
the parity run surfaced (19 text/text-on-shape), each parity-gated with a 0038 reason.
**Postmortem + hoist proposal to be executed after the genuine bugs are root-caused.**

## Thesis

`RendererGeode::drawText` and `RendererTinySkia::drawText` are **two parallel
reimplementations of the same logic**. Each independently re-derives:

- **(a) per-glyph placement transform** — translate / rotate / stretch order, plus
  per-character `dy` / `rotate` lists and `textPath` advancement;
- **(b) span paint resolution** — fill / stroke / opacity, solid vs gradient vs pattern,
  default-fill black, `currentColor`, `objectBoundingBox`-via-text-bbox;
- **(c) decoration geometry** — underline / overline / line-through rects from font
  metrics, and their paint (which inherits the span paint);
- **(d) font-size resolution** — named values (`xx-small`…), percent, negative sizes.

**Every parity gap in the catalog below is a place these two re-derivations drifted.**
Fixing them one-by-one in `RendererGeode::drawText` re-converges the two copies
temporarily, but the copies will drift again. The durable fix is to **hoist (a)–(d) into a
shared layer** (above both backends, below `TextEngine`) that emits backend-agnostic draw
ops; each backend's `drawText` becomes a thin consumer. See "Hoist proposal" below.

## Divergence catalog

Legend — **Where:** the layer the divergence currently lives in. **Hoist:** does the
durable fix belong in the proposed shared layer (Y), or is it genuinely backend-specific (N)?

### Found + fixed this session (each was a geode-only re-derivation gap)

| # | Divergence | geode (before) vs tiny | Fix commit | Where | Hoist |
|---|---|---|---|---|---|
| D1 | text-decoration not drawn | geode drew no underline/overline/line-through; tiny did | `d1742348c` | (c) decoration geometry | **Y** |
| D2 | stroked-glyph ring fill rule | geode used `NonZero` (filled interior solid); ring needs `EvenOdd` | `2314efb0d` | (b)/(a) stroke→fill | **Y** |
| D3 | pattern-fill on text | geode `drawText` had no pattern path → glyphs unfilled + staged `patternFillPaint` slot leaked to next shape | `1e2eb2b6f` | (b) paint resolution | **Y** |
| D4 | stretch+rotate transform order | tiny: stretch-on-outline → `Rotate*Translate`; geode: `Scale*Rotate*Translate` — diverge when `stretchScale≠1` **and** `rotate≠0` | latent (no test) | (a) placement transform | **Y** |

### Open — genuine text divergences (parity-gated, reason → this doc)

From the **Phase 4b whole-suite parity run** (geode↔tiny-skia at the policy metric: per-pixel
`kDefaultThreshold` 0.02, `includeAA=false`; px = diff count). **Audited 2026-05-26** by
eyeballing every diff PNG (geode + tiny + diff) — magnitude alone is *not* decisive
(B10 is 3488px yet edge-floor; a baseline-shift offset is kilo-pixel AND structural).
Classified by **where the diff lands**:

- **STRUCTURAL** — geode renders *wrong* (whole-glyph offset / wrong paint / missing text);
  solid-region diff. Stays in `kGenuineText`; the hoist's paint(b) + baseline-shift/dy-rotate
  consume increments target these. **14 tests.**
- **EDGE-FLOOR (was mislabeled)** — geode renders *correct* (right glyphs/positions/colors);
  the diff is the thin 4× MSAA fringe, over 100px only from large cumulative perimeter (many
  lines / long strings / tiled or on-path small text). Moved to `kEdgeFloor`. **5 tests.**

| # | px | test | class | symptom (eyeballed) | layer | Hoist |
|---|---|---|---|---|---|---|
| B1 | 19750 | `text/baseline-shift/nested-with-baseline-2` | **STRUCT** | tiny draws 2 strings (black shifted + red reset); **geode draws only 1 unshifted** — nested baseline-shift ignored | (a) baseline-shift | **Y** |
| B2 | 12886 | `text/baseline-shift/nested-with-baseline-1` | **STRUCT** | same: geode drops the shifted/reset string | (a) baseline-shift | **Y** |
| B3 | 4338 | `text/baseline-shift/mixed-nested` | **STRUCT** | center+right "Text" solid-offset vs tiny | (a) baseline-shift | **Y** |
| B4 | 4320 | `text/baseline-shift/deeply-nested-super` | **STRUCT** | C/D/E/F progressively wrong super-shift (A/B match) | (a) baseline-shift | **Y** |
| B5 | 2870 | `text/baseline-shift/nested-super` | **STRUCT** | rightmost "Text" solid-offset (left two match) | (a) baseline-shift | **Y** |
| B6 | 2438 | `text/baseline-shift/nested-length` | **STRUCT** | rightmost "Text" solid-offset | (a) baseline-shift | **Y** |
| B7 | 4643 | `text/text-decoration/underline-with-dy-list-2` | **STRUCT** | per-char `dy` staircase differs; whole-glyph vertical offset | (a) per-char dy | **Y** |
| B8 | 4561 | `text/text-decoration/underline-with-rotate-list-4` | **STRUCT** | per-char rotate list differs; glyphs rotated to wrong angles | (a) per-char rotate | **Y** |
| B11 | 2929 | `text/tspan/tspan-bbox-2` | **STRUCT** | both lines solid fill-color diff (paint via text bbox) | (b) bbox paint | **Y** |
| B12 | 1803 | `text/tspan/tspan-bbox-1` | **STRUCT** | "long" span solid fill-color diff (bbox paint) | (b) bbox paint | **Y** |
| B15 | 14562 | `painting/fill/radial-gradient-on-text` | **STRUCT** | gradient fill ramp differs across whole glyph bodies | (b) bbox paint | **Y** |
| B16 | 13115 | `painting/stroke/pattern-on-text` | **STRUCT** | pattern stroke tiling/placement differs | (b) paint resolution | **Y** |
| B17 | 11917 | `painting/stroke/linear-gradient-on-text` | **STRUCT** | linear-gradient stroke ramp differs | (b) bbox paint | **Y** |
| B18 | 10195 | `painting/fill/linear-gradient-on-text` | **STRUCT** | linear-gradient fill ramp differs | (b) bbox paint | **Y** |
| ~~B9~~ | 1822 | `text/text-decoration/tspan-decoration` | EDGE | geode renders correct multi-color text + underlines; thin fringe on long small string | — | n/a |
| ~~B10~~ | 3488 | `text/font-size/named-value` | EDGE | 11 small (size-12) lines render correct; cumulative edge fringe | — | n/a |
| ~~B13~~ | 2219 | `text/textPath/dy-with-tiny-coordinates` | EDGE | glyphs correctly on-path; fringe + 0.5px path line | — | n/a |
| ~~B14~~ | 932 | `text/letter-spacing/on-Arabic` | EDGE | correct glyphs/spacing; fringe + ref lines | — | n/a |
| ~~B19~~ | 1663 | `paint-servers/pattern/text-child` | EDGE | `<pattern>` tiled with text renders correct; fringe over tiled field | — | n/a |

**Structural shared roots (the hoist targets):** **B1–B6** baseline-shift nesting (geode
ignores/mis-applies nested baseline-shift — the largest, most clearly-broken cluster);
**B11/B12/B15/B17/B18** per-span / text-bbox gradient paint resolution; **B16** pattern-on-
text paint; **B7/B8** per-char `dy`/`rotate` list consumption. (B16 is pattern *as the text
fill*, structural; the EDGE B19 `pattern/text-child` is a `<pattern>` *containing* text,
which renders correctly.)

> Surprise / contradiction to flag: B1/B2 show geode **drops an entire baseline-shifted
> string**, and B7/B8 show whole-glyph dy/rotate offsets — yet increment-2 analysis found
> `glyph.{x,y,rotate}` *identical* between backends (shared `runs` cache). So the baseline-
> shift / dy-rotate divergence is **not** in the shared glyph positions — it's in a
> per-backend consume path (decoration/second-pass placement, or geode re-deriving shift)
> that the increment-3 (already-shared font-size/scale) finding did not cover. The
> baseline-shift increment must locate where geode diverges despite identical layout input.

> Note: the strict-0 characterization listed `font-size/negative-size` (5588) and
> `tspan/with-opacity` (1599) as bugs; both drop below the 100-px flat budget at 0.02 and are
> NOT gated.

> Every STRUCTURAL divergence is a **Hoist = Y** — there is no backend-specific reason for
> any of them to differ.

### Open — non-text filter divergences (parity-gated, reason → 0021 §G2)

The parity run also surfaced **37 pure-filter** geode↔tiny divergences (gated with a
0021-G2 reason). These are NOT drawText gaps and are out of this doc's hoist scope; listed
here only for completeness of the parity gate ledger. By theme (px = geode↔tiny at 0.02):

- **feTurbulence (12)** — Perlin-noise pattern genuinely differs per pixel (different noise
  impl); ~47k–89k px. Algorithmic parity, not a uniform offset.
- **feImage (9)** — subregion / transform / opacity / chained placement; ~1.5k–88k.
- **feComposite arithmetic (4)** — visibly more-saturated output vs tiny; 160k–230k.
- **feComponentTransfer (4)** — table/linear transfer wrong output color; 160k.
- **feMerge (2)**, **feColorMatrix (1)**, **feConvolveMatrix (1)**, **feDiffuseLighting (1)**,
  **feSpecularLighting (1)**, **feGaussianBlur/complex-transform (1)**,
  **filter/on-group-with-child-outside-of-canvas (1)**.

These overlap the [0021 §G2](0021-resvg_feature_gaps.md#g2-filter-primitive-correctness-16-of-23-disabled-tests)
filter-correctness backlog; fix there, then drop the parity gates.

### The 137 sub-visual premultiply fills (NOT gated — pass at 0.02)

At strict-0, ~137 non-text tests showed a whole-fill diff that **collapses to <100 px at
the policy 0.02 threshold** — a uniform, sub-perceptual color/alpha offset across solid
fills (premultiplied-alpha / color-space rounding between geode and tiny). They PASS parity
and are not gated. Tracked as a single root-cause item in
[0021 §G5](0021-resvg_feature_gaps.md#g5-audit-the-aa-justified-geode-thresholds) (likely one premultiply/rounding fix clears most).

## Hoist proposal (post-matrix)

Introduce a shared **`PlacedText`** builder in the renderer layer (consumed by both
`RendererGeode::drawText` and `RendererTinySkia::drawText`). Given a
`ComputedTextComponent` + `TextParams`, it produces a backend-agnostic op list:

- **glyph ops**: `{ placed outline Path (already transformed for translate/rotate/stretch +
  dy/rotate lists), resolved fill (solid/gradient/pattern), fill rule, resolved stroke +
  stroke style }`;
- **decoration ops**: `{ rect Path, resolved fill, resolved stroke }`.

All of (a)–(d) live in the builder, computed **once**. Each backend's `drawText` collapses
to a loop calling `fillPath` / `fillPathPattern` / `strokeToFill` — no placement math, no
paint resolution, no decoration geometry per backend. The parity gaps above become
structurally impossible (one implementation can't disagree with itself), and the parity
test's job shrinks to "did the rasterizer rasterize the identical ops the same way" — i.e.
purely the 4× MSAA edge floor.

**Sequencing:** land the backend matrix + parity test first (so the catalog is complete
and we have a green parity gate to refactor under), root-cause the 9 bugs (filling in the
catalog), *then* do the hoist as its own refactor — each catalogued divergence becomes a
regression test that must stay green through the hoist. Do the hoist in-place (modify both
`drawText`s to call the shared builder incrementally), not as a parallel new path
(CLAUDE.md: no dead code, refactor in-place).

## Concrete increment plan (refined 2026-05-25)

**Invariant for every increment: tiny-skia text output stays byte-identical.** The shared
layer encodes tiny-skia's *current* behavior (the parity reference). tiny adopts each slice
at zero change; geode converges to it in a later step, which is when the `kGenuineText`
parity gates flip. Verify byte-identity after each tiny-adopting step against `:resvg_test_suite`
(default + max) text tests and `:renderer_tests`.

Key facts that shape the order (confirmed in code):
- Both backends already share the **same** `runs` (`ComputedTextGeometryComponent` cache)
  and `toTextLayoutParams`, so `glyph.{xPosition,yPosition,rotateDegrees,stretchScale,
  fontSizeScale}` are **identical** between backends. The 19 divergences are therefore in
  how each backend **consumes** those values (placement transform, paint resolution,
  decoration geometry, per-run font-size/scale) — exactly what the builder consolidates.
- The two backends' free-function `transformPath(Path, Transform2d)` are byte-identical
  duplicates; the placement slice also removes that duplication.

**Increment order (each lands green on its own):**

1. ✅ **Placement geometry (a) — `PlacedTextGeometry` / `placedGlyphOutline`** (`f06f717f8`).
   Shared pure-geometry helper (lib code, no backend/paint types): given `TextEngine`, a
   run's `FontHandle`+`scale`, and a `TextGlyph`, returns the placed outline `Path` encoding
   tiny-skia's exact sequence: `glyphOutline(font, idx, scale*fontSizeScale)` → stretch on
   the **raw outline** (`Scale(stretchX,stretchY)`) → `Rotate(rotateDegrees) * Translate(x,y)`.
   Also provides the shared `transformPath`. tiny adopted at zero behavior change (goldens
   byte-identical); geode untouched in that step.
2. ✅ **Geode adopts placement** (increment 2). `RendererGeode::drawText` calls
   `placedGlyphOutline` + shared `transformPath`; geode's duplicate `transformPath` removed.
   **Outcome: this fixes D4 structurally but flips ZERO parity gates** — measured (gates
   bypassed) every gated test's geode↔tiny px is *unchanged* (whole-suite parity stays
   1035/228, GeodeGolden stays 1046, all green). **Finding: D4 is purely latent** — no test
   in the suite has simultaneous `stretchScale≠1` *and* `rotateDegrees≠0`, so the
   order-of-operations fix changes no pixels. The placement transform is *not* the cause of
   any of the 19 text divergences; their glyph `{x,y,rotate,stretch}` are identical between
   backends (shared `runs` cache), so the divergence is in the *other* consume paths (per-run
   scale/font-size, paint resolution, decoration, baseline-shift consumption) — increments
   3–5. Increment 2 is still the right structural step (de-dups `transformPath`, makes D4
   impossible for any future stretch+rotate test, and converges the code path geode 3–5 build
   on), but it is a no-op at the pixel level. **← THIS INCREMENT (no gates flipped; none
   un-gated, per "un-gate only on measured ≤100px pass").**
3. ~~**Per-run scale + font-size resolution (d).**~~ **SKIPPED — already shared.** Audited:
   `spanFontSizePx` (element + per-span override, named/percent values) and
   `scaleForPixelHeight` are computed by a **byte-identical** expression in both backends
   (same inputs), so there is nothing to extract — it flips no gates. B10
   (`font-size/named-value`) was *not* a font-size bug at all: the named keywords are on the
   `<rect>`s, the text is all size-12, geode renders it correctly, and the 3488px is edge
   fringe over 11 small lines → reclassified **edge-floor** (gate moved to `kEdgeFloor`).
   The next real consume-path increment is paint resolution below.
4. **Paint resolution (b) — `resolveSpanFill` / stroke / opacity / bbox.** The builder
   resolves each run's fill (solid/gradient/pattern), stroke, and the `objectBoundingBox`
   text bbox into backend-agnostic descriptors (color + a paint-server handle + bbox), so
   both backends map the *same* resolved paint to their own shader/pipeline. *Subsumes:*
   B11/B12/B15/B17/B18 (per-span / text-bbox paint), B16/B19 (pattern on text), and the
   137-fill premultiply only insofar as inputs match (the rounding itself is rasterizer-side,
   0021 G5).
4. **Decoration geometry (c).** Hoist underline/overline/line-through rect derivation +
   its paint inheritance into the builder. *Subsumes:* B9 (decoration drift), B7/B8
   decoration half.
5. **textPath + baseline-shift consume paths.** Whatever placement nuance remains for
   `textPath` advance (B13) and baseline-shift nesting (B1–B6) once (a)+(d) are shared.
6. **Collapse both `drawText`s to thin op consumers.** Final state: each backend loops the
   builder's glyph/decoration ops calling `fillPath`/`fillPathPattern`/`strokeToFill`. Drop
   the subsumed `kGenuineText` entries from `resvg_test_suite.cc` and mark ✅ here, verifying
   each parity gate flips green.

**Increment 1 detail:** introduced `donner/svg/renderer/PlacedTextGeometry.{h,cc}`
(lib target) with `transformPath` + `placedGlyphOutline`; `RendererTinySkia::drawText` calls
`placedGlyphOutline` instead of its inline outline→stretch→translate→rotate block, and uses
the shared `transformPath`. Zero behavior change (same sequence factored out). Geode not
touched. Proof = tiny text goldens byte-identical (91-test before/after count diff = 0).

**Increment 2 detail:** `RendererGeode::drawText` now calls `placedGlyphOutline` (replacing
its `Scale*Rotate*Translate` `glyphFromLocal` block) and the shared `transformPath`; geode's
duplicate `transformPath` removed (both backends now share one definition). Geode's
per-glyph bitmap-only-font skip (run-level) and `.notdef` skip are preserved
(`placedGlyphOutline` returns empty for outline-less glyphs). **Verified geode output is
byte-identical to pre-increment**: whole-suite parity 1035 ≤100 / 228 >100 (unchanged),
GeodeGolden 1046 pass (unchanged), all 3 modes FAIL=0. **0 of the 228 gates flipped** — the
D4 order fix is latent (no test triggers stretch+rotate together), so no `kGenuineText` /
`kEdgeFloor` entry was removed. The real text divergences move in increments 3–5 (scale /
paint / decoration / baseline-shift consume paths). Gate ledger then was 19 text + 37 G2 +
172 edge-floor = 228.

**Audit detail (2026-05-26 — triage pass, ledger-only, no refactor):** eyeballed all 19
`kGenuineText` diff PNGs → 14 STRUCTURAL (stay) / 5 EDGE-FLOOR mislabeled (moved to
`kEdgeFloor`): `font-size/named-value`, `text-decoration/tspan-decoration`,
`textPath/dy-with-tiny-coordinates`, `letter-spacing/on-Arabic`, `paint-servers/pattern/
text-child`. Also confirmed (increment-3 finding) per-run font-size/scale is already
byte-identical between backends — no extraction needed. **Gate ledger now: 14 text + 37 G2 +
177 edge-floor = 228** (total unchanged; all green; only the skip *reason* changed for the 5
moved). The 14 STRUCTURAL are the explicit hoist-target list for the paint(b) +
baseline-shift/dy-rotate consume increments.
