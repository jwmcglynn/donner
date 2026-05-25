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

Final list from the **Phase 4b whole-suite parity run** (geode↔tiny-skia at the policy
metric: per-pixel `kDefaultThreshold` 0.02, `includeAA=false`; px = diff count). All are
gated in the `GeodeTinyParity` mode via `geodeParityGate` (reason references 0038) and stay
green; root causes TBD. **19 tests** — 14 under `text/`, plus 5 text-rendering cases that
sit under paint directories (`*-on-text` / `pattern text-child`) but are the same drawText
re-derivation gaps.

| # | px | test | symptom (eyeballed) | suspected layer | Hoist |
|---|---|---|---|---|---|
| B1 | 19750 | `text/baseline-shift/nested-with-baseline-2` | nested baseline-shift offset accumulates wrong | (a) baseline-shift | **Y** |
| B2 | 12886 | `text/baseline-shift/nested-with-baseline-1` | same family, single nest | (a) baseline-shift | **Y** |
| B3 | 4338 | `text/baseline-shift/mixed-nested` | mixed sub/super nest offset | (a) baseline-shift | **Y** |
| B4 | 4320 | `text/baseline-shift/deeply-nested-super` | deep super-nest offset | (a) baseline-shift | **Y** |
| B5 | 2870 | `text/baseline-shift/nested-super` | super-nest offset | (a) baseline-shift | **Y** |
| B6 | 2438 | `text/baseline-shift/nested-length` | length-based nest offset | (a) baseline-shift | **Y** |
| B7 | 4643 | `text/text-decoration/underline-with-dy-list-2` | per-char `dy` list applied differently; doubled outlines | (a) per-char dy | **Y** |
| B8 | 4561 | `text/text-decoration/underline-with-rotate-list-4` | per-glyph rotate-list drift | (a) per-char rotate | **Y** |
| B9 | 1822 | `text/text-decoration/tspan-decoration` | glyph+decoration drift across styled spans | (a)+(c) | **Y** |
| B10 | 3488 | `text/font-size/named-value` | named-size (`xx-small`…) lines vertically offset, accumulating | (d) font-size resolution | **Y** |
| B11 | 2929 | `text/tspan/tspan-bbox-2` | solid fill-color diff (paint via text bbox) | (b) bbox paint | **Y** |
| B12 | 1803 | `text/tspan/tspan-bbox-1` | solid fill-color diff (same family as B11) | (b) bbox paint | **Y** |
| B13 | 2219 | `text/textPath/dy-with-tiny-coordinates` | glyphs drift along path (tiny `dy`) | (a) textPath dy | **Y** |
| B14 | 932 | `text/letter-spacing/on-Arabic` | letter-spacing applied differently on Arabic shaping | (a) letter-spacing | **Y** |
| B15 | 14562 | `painting/fill/radial-gradient-on-text` | radial-gradient fill on text — bbox/paint diff | (b) bbox paint | **Y** |
| B16 | 13115 | `painting/stroke/pattern-on-text` | pattern stroke on text | (b) paint resolution | **Y** |
| B17 | 11917 | `painting/stroke/linear-gradient-on-text` | linear-gradient stroke on text | (b) bbox paint | **Y** |
| B18 | 10195 | `painting/fill/linear-gradient-on-text` | linear-gradient fill on text | (b) bbox paint | **Y** |
| B19 | 1663 | `paint-servers/pattern/text-child` | `<pattern>` with a text child | (b) paint resolution | **Y** |

Likely shared roots: **B1–B6** (one baseline-shift nesting bug — the largest cluster, newly
surfaced by parity; was not in the strict-0 list); **B11+B12+B15+B17+B18** (per-span /
text-bbox paint resolution — gradient/pattern-on-text); **B7+B8** (per-char `dy`/`rotate`
list consumption).

> Note: the strict-0 characterization listed `font-size/negative-size` (5588) and
> `tspan/with-opacity` (1599) as bugs; both drop below the 100-px flat budget at the policy
> 0.02 threshold (negative-size: geode's near-empty render matches tiny's within tolerance;
> with-opacity: a sub-visual alpha offset). They are NOT gated.

> Every catalogued divergence is a **Hoist = Y** — which is itself the argument for the
> shared layer: there is no backend-specific reason for any of these to differ.

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
