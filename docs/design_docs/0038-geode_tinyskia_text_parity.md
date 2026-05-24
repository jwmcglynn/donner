# 0038 — Geode ↔ tiny-skia text parity: divergence catalog + hoist-shared-drawText proposal

**Status:** living catalog (opened 2026-05-24). Accumulates every geode↔tiny-skia text
divergence found during the [Phase 4b](0017-geode_renderer.md#phase-4b-in-process-backend-matrix--geode-vs-tiny-skia-parity-comparison)
parity push, then proposes the structural fix. **Postmortem + hoist proposal to be
executed after the backend-matrix lands and the 9 genuine bugs are root-caused.**

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

### Open — the 9 genuine bugs (geode↔tiny-skia ≥1599px, from Phase 4b characterization)

Root causes TBD in increment 6; fill in the "geode vs tiny" + "Where" columns as each is
repro'd. px = geode↔tiny-skia diff at threshold 0 / `includeAA=false`.

| # | px | test | symptom (eyeballed) | suspected layer | Hoist |
|---|---|---|---|---|---|
| B1 | 5588 | `font-size/negative-size` | geode draws nothing; tiny draws mirrored "Text" | (d) font-size sign | **Y** |
| B2 | 3586 | `font-size/named-value` | 11 named-size lines vertically offset, accumulating | (d) font-size resolution | **Y** |
| B3 | 4767 | `text-decoration/underline-with-dy-list-2` | per-char `dy` list applied differently; doubled outlines | (a) per-char dy | **Y** |
| B4 | 4605 | `text-decoration/underline-with-rotate-list-4` | per-glyph rotate-list drift | (a) per-char rotate | **Y** |
| B5 | 2271 | `text-decoration/tspan-decoration` | glyph+decoration drift across styled spans | (a)+(c) | **Y** |
| B6 | 2929 | `tspan/tspan-bbox-2` | solid fill-color diff (paint via text bbox) | (b) bbox paint | **Y** |
| B7 | 1805 | `tspan/tspan-bbox-1` | solid fill-color diff (same family as B6) | (b) bbox paint | **Y** |
| B8 | 1599 | `tspan/with-opacity` | span renders at different alpha | (b) per-span opacity | **Y** |
| B9 | 2228 | `textPath/dy-with-tiny-coordinates` | glyphs drift along path (tiny `dy`) | (a) textPath dy | **Y** |

Likely shared roots: **B6+B7+B8** (per-span paint/opacity/bbox resolution — one fix may
clear all three); **B3+B4** (per-char `dy`/`rotate` list consumption). B1 is the cleanest
single check (geode emits zero glyphs).

> Every catalogued divergence so far is a **Hoist = Y** — which is itself the argument for
> the shared layer: there is no backend-specific reason for any of these to differ.

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
