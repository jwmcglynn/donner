# 0040 — Geode Slug-algorithm conformance: divergences from the published method

**Status:** living conformance tracker (opened 2026-05-25). Records where Geode's
implementation of the **Slug algorithm** deviates from the published algorithm, so the
divergences are tracked and closed deliberately rather than discovered as parity bugs.
Complements [0039](0039-geode_analytical_aa.md) (the analytical-AA coverage rollout) and
the parity ledger in [0038](0038-geode_tinyskia_text_parity.md) / [0021](0021-resvg_feature_gaps.md).

## Source of truth

Audited Geode's `donner/svg/renderer/geode/` Slug pipeline against:
- Eric Lengyel, **"GPU-Centered Font Rendering Directly from Glyph Outlines,"** JCGT 6(2),
  2017 — https://jcgt.org/published/0006/02/02/
- **Slug library** — https://sluglibrary.com/ ; **"A Decade of Slug"** —
  https://terathon.com/blog/decade-slug.html
- Author-endorsed reference impl **mightycow/Sluggish** (`SlugPixelShader.hlsl` —
  classification code `0x2E74`, root extraction, coverage accumulation, dual-ray combine).

Geode "implements the Slug algorithm": per-pixel, cast a ray, find Bézier/ray crossings,
accumulate winding/coverage from band-binned curves. The audit checked banding, root-find,
curve classification, quadratic/cubic handling, fill rule, the per-pixel scale, and data
encoding. The per-pixel **AA coverage** math (Eq. 3 / dual-ray) is being (re)implemented in
[0039](0039-geode_analytical_aa.md) and is tracked there; this doc covers the rest.

## Divergence ledger

Severity: **Critical** = correctness/parity bug with broad blast radius; **Moderate** =
real divergence, narrower or conditional; **Benign** = legitimate adaptation or
already-correct.

| # | Sev | Area | Geode location | Status |
|---|-----|------|----------------|--------|
| D1 | Critical | gradient/mask/alpha-coverage pipelines still run a non-Slug single-direction integer-winding tracer (`curve_winding`) + cancellation-prone `solve_quadratic` | `slug_gradient.wgsl:225`, `slug_mask.wgsl:164`, `slug_fill_alpha_coverage.wgsl:165` | **queued** (converge after fill tracer lands, [0039](0039-geode_analytical_aa.md) step 5) |
| D2 | Critical (latent) | encoder `CurveTo` silently collapses a cubic to a straight chord (midpoint control pt) instead of subdividing | `GeodePathEncoder.cc:184` | **backlog** — subdivide-or-assert |
| D3 | Critical | pixels-per-em scale `m` uses `abs(dpdx().x)`/`abs(dpdy().y)`, not `length(dpdx())`/`fwidth` — overestimates `m` under rotation/sub-pixel features | `slug_fill.wgsl:439` | **in flight** (0039 AA increment) |
| D4 | Moderate | band binning by control-point AABB over-includes curves; no per-band sort | `GeodePathEncoder.cc:253` | open (perf; benign correctness) |
| D5 | Moderate | even-odd implemented as a triangle-wave fold of *fractional* coverage, not integer crossing parity | `slug_fill.wgsl:340` | **queued** (decide with 0039 rollout) |
| D6 | Moderate | degeneracy threshold `abs(a)<1e-4` is absolute (path-space units), not scale-relative; no `b≈0` NaN guard on the linear fallback | `slug_fill.wgsl:236`, `:319` | **in flight** (0039 AA increment) |
| D7 | Benign | classification index uses `0x2E74>>idx` shift vs reference sign-bit form — numerically equivalent (verified all 8 sign configs); only differs at the measure-zero `y==0` boundary | `slug_fill.wgsl:304` | no action |
| D8 | Benign | half-pixel dilation uses the orthographic-2D simplification, not the full perspective quadratic — correct for Geode's affine 2D content | `slug_fill.wgsl:182` | no action |
| D9 | Benign | lines encoded as degenerate quads (control pt = midpoint) — the standard Slug approach | `GeodePathEncoder.cc:147` | no action |
| D10 | Benign | implicit-close of open subpaths — correct SVG-fill conformance adaptation (Slug assumes closed contours) | `GeodePathEncoder.cc:116` | no action |
| D11 | Benign | ~32px/band, max 256 — implementation-defined banding granularity | `GeodePathEncoder.cc:37` | no action |
| D12 | Benign | f32 SSBO curve storage vs the reference's f16 textures — more precise, legitimate WebGPU adaptation | `slug_fill.wgsl:91` | no action |

## The real divergences (detail)

### D1 — three of four pipelines run a non-Slug winding algorithm
Only `slug_fill.wgsl` is being moved to the analytical classification tracer. `slug_gradient.wgsl`,
`slug_mask.wgsl`, and `slug_fill_alpha_coverage.wgsl` still use `curve_winding`: a one-directional
scanline winding count (`if (x < sample.x) continue;`) with **no `0x2E74` root-eligibility
classification** and the cancellation-prone `(-b ± √disc)/2a` form of `solve_quadratic`. Symptoms:
(a) gradient/clip-mask/Intel-fallback coverage diverges from the solid-fill path for identical
geometry; (b) dropped/double-counted crossings at sharp joins and vertical tangents that the Slug
classification code specifically prevents. **Close:** converge all four onto the shared analytical
tracer (or at minimum the robust `solve_quadratic` + classification eligibility) once the fill
tracer is finalized. Largest blast radius; do after [0039](0039-geode_analytical_aa.md) steps 1–3.

### D2 — cubic CurveTo collapses to a chord (latent landmine)
`extractCurves`'s `CurveTo` case discards both cubic control points and emits a quad with control
point = endpoint midpoint = a straight line. Dead in current call paths (`cubicToQuadratic` runs
first for both band sets), but any future caller handing a raw cubic to `GeodePathEncoder::encode`
silently flattens every cubic with no error. **Close:** subdivide the cubic into quads (the Slug
requirement) or hard-fail; never silently produce wrong geometry.

### D3 — per-pixel scale ignores rotation/shear
`m = 1/abs(dpdx(sample_pos).x)` (and `.y`) takes only the axis-aligned derivative component;
the reference uses the full gradient magnitude (`1/fwidth` ≈ `1/length(dpdx)`). Since
`abs(dx.x) ≤ length(dx)`, `m` is overestimated → coverage ramp too steep, worst on thin/rotated
features (glyph stems). Independently matches the AA increment's observed text-glyph regression.
**Close (in flight):** `m = 1/max(length(dpdx(sample_pos)), 1e-8)` per ray.

### D5 — even-odd as a triangle-wave fold
`apply_fill_rule` folds the continuous coverage sum with a period-2 triangle wave for even-odd;
the spec/reference define even-odd on integer crossing parity. Wrong where two edges' fractional
coverages overlap within a pixel. The integer-winding shaders (D1) get even-odd correct via
`(winding & 1)`, so Geode currently has two even-odd behaviors. **Close:** decide the analytical
even-odd policy alongside the 0039 coverage rework.

### D6 — absolute degeneracy threshold + missing NaN guard
`abs(a) < 1e-4` is in raw path-space units, so genuine shallow quads at large coordinates (or
near-degenerate lines at tiny coordinates) misfire the linear fallback; and a quad with both
`a≈0` and `b≈0` divides by ~0 → NaN coverage (the AA increment's text-glyph garbage). **Close
(in flight):** make the threshold scale-relative (vs control-point span / `max(|a|,|b|,|c|)`)
and guard the linear fallback against `b≈0`.

## Sequencing

D3 + D6 are the analytical-AA text-glyph blocker → closing now inside the [0039](0039-geode_analytical_aa.md)
steps 1–3 increment. The rest are sequenced **after** steps 1–3 land, because they touch the
same files the AA work is actively editing (or depend on the finalized fill tracer):

1. **D3, D6** — in flight (AA increment; unblocks the edge-floor gate flips).
2. **D1** — converge gradient/mask/alpha-coverage onto the shared analytical tracer (highest
   blast radius; also flips the gradient/mask edge-floor gates that steps 1–3 leave gated).
3. **D5** — settle analytical even-odd.
4. **D2** — encoder cubic subdivide-or-assert.
5. **D4** — optional: tighter band binning by true curve extent.

Each close lands in-place, green, with the geode-vs-tiny parity suite (and the geode golden
suite) as the gate; un-gate edge-floor entries only when they measure ≤100 px.
