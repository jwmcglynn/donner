# 0040 — Geode Slug implementation: developer reference

**Status:** Developer reference. Describes Geode's as-built implementation of the
**Slug algorithm** (per-pixel ray/Bézier-crossing winding) — the pipeline a contributor
must understand to work on the fill/coverage shaders or the path encoder — plus the
invariants it relies on and the known limitations (D3/D4/D6) that remain future work.
The per-pixel **AA coverage** math is documented in [0039](0039-geode_analytical_aa.md);
this doc covers the rest of the pipeline.

**Related:** [0039 anti-aliasing](0039-geode_analytical_aa.md),
[0038 text parity](0038-geode_tinyskia_text_parity.md),
[0021](0021-resvg_feature_gaps.md)

## Source of truth

Geode implements the Slug algorithm clean-room (ISC). The algorithmic references are:
- Eric Lengyel, **"GPU-Centered Font Rendering Directly from Glyph Outlines,"** JCGT
  6(2), 2017 — https://jcgt.org/published/0006/02/02/
- **Slug library** — https://sluglibrary.com/ ; **"A Decade of Slug"** —
  https://terathon.com/blog/decade-slug.html
- Author-endorsed reference impl **mightycow/Sluggish** (`SlugPixelShader.hlsl` —
  `0x2E74` classification, root extraction, coverage accumulation).

The patent (US10373352B1) was dedicated to the public domain on 2026-03-17; reference
shaders are MIT. Geode's implementation is independent.

---

## 1. The as-built Slug pipeline

Geode renders a `Path` by encoding it into band-binned Y-monotonic quadratic curves on
the CPU, then per-pixel casting a horizontal ray and counting winding from the curves in
the pixel's band. Two halves:

### 1.1 CPU encoder — `GeodePathEncoder.cc`

`GeodePathEncoder::encode` lowers a `Path` into an `EncodedPath` of bands + quadratic
curves for the GPU:

1. **Cubic lowering (invariant):** `encode` runs `path.cubicToQuadratic(tolerance)`
   first (`GeodePathEncoder.cc:56`), so `extractCurves` only ever sees `QuadTo`
   segments. A raw `CurveTo` reaching `extractCurves` is a hard-fail
   (`UTILS_RELEASE_ASSERT_MSG`, `GeodePathEncoder.cc:185`) — the "cubic-free input"
   invariant is documented in code rather than silently flattened (D2).
2. **Lines → degenerate quads:** a `LineTo` becomes a quadratic with control point at
   the segment midpoint (`GeodePathEncoder.cc:147`) — the standard Slug approach (D9).
3. **Implicit close:** open subpaths are implicitly closed for winding correctness
   (`GeodePathEncoder.cc:116`); explicit `Close` suppresses the final implicit close.
   Stroke geometry funnels through `Path::strokeToFill` (already closed), so the encoder
   sees a pre-closed ribbon there (D10).
4. **Banding:** the path's Y range is split into bands of **~32 px each, max 256 bands**
   (`computeBandCount`, `GeodePathEncoder.cc:44`); a Y-monotonic curve is duplicated into
   every band its Y-extent overlaps (`GeodePathEncoder.cc` step 5, ~line 229). Band
   binning currently uses the control-point AABB and is unsorted within a band (D4).
5. **Encoding:** curves are stored as **f32** in an SSBO (more precise than the
   reference's f16 textures — a legitimate WebGPU adaptation, D12).

### 1.2 GPU fragment shader — `slug_fill.wgsl`

Per pixel, `fs_main` casts a horizontal ray and counts winding from the band's curves:

1. **Per-pixel scale:** reads `dpdx`/`dpdy` of `sample_pos` (`slug_fill.wgsl:393`),
   constant across the affine primitive (D3 — see limitations).
2. **Half-pixel dilation:** the orthographic-2D simplification (`slug_fill.wgsl:182`),
   correct for Geode's affine 2D content (D8).
3. **Root-find per curve:** `curve_winding(curve, sample)` (`slug_fill.wgsl:279`) forms
   the quadratic in the ray parameter and calls `solve_quadratic` (`slug_fill.wgsl:220`),
   which uses the **numerically-stable Citardauq form** (`q = -(b + sign(b)·√disc)/2`,
   `t0 = q/a`, `t1 = c/q`, with a `q≈0` double-root fallback). Degenerate
   `abs(a) < 1e-4` routes to the linear fallback (`slug_fill.wgsl:233`) (D6 — see
   limitations).
4. **Crossing eligibility + direction:** `curve_winding` (`slug_fill.wgsl:286-308`) counts
   a root only when `t ≥ 0` **and** the crossing is to the right of the sample
   (`x(t) ≥ sample.x` — a one-directional horizontal ray); the contribution is the sign of
   the tangent `dy/dt` at `t` (±1). There is **no** Slug `0x2E74` classification tracer on
   the clean tree — that analytical approach is in
   [0039 rejected approaches](0039-geode_analytical_aa.md) (the audit's "classification"
   reading was a mid-edit artifact; see appendix).
5. **Winding accumulation + fill rule:** `sample_is_inside` (`slug_fill.wgsl:339`) sums
   `curve_winding` over the band's curves and applies the fill rule: **non-zero**
   = `winding != 0`, **even-odd** = `(winding & 1) != 0` — integer crossing parity,
   evaluated per sub-pixel sample.
6. **Coverage:** the winding test runs at 4 sub-pixel samples packed into
   `@builtin(sample_mask)` (the 4× MSAA path). The coverage/AA model is documented in
   [0039](0039-geode_analytical_aa.md).

All six Slug shaders (`slug_fill`, `slug_gradient`, `slug_mask`, and the three
vendor-gated `*_alpha_coverage` variants) run the **same** integer-winding
`curve_winding` tracer and the same `solve_quadratic`.

---

## 2. Conformance ledger

Severity: **Critical** = correctness/parity bug, broad blast radius; **Moderate** =
narrower or conditional; **Benign** = legitimate adaptation or already-correct. Most are
resolved (implementation notes / invariants in §1); the open ones are the known
limitations in §3.

| # | Sev | Area | Geode location | Status |
|---|-----|------|----------------|--------|
| D1 | Critical | `solve_quadratic` numerical stability | all six Slug shaders | **resolved** — Citardauq form (§1.2 step 3) |
| D2 | Critical (latent) | encoder must not silently flatten cubics | `GeodePathEncoder.cc:185` | **resolved** — hard-fail assert documents the cubic-free invariant (§1.1) |
| D3 | Critical | pixels-per-em scale ignores rotation/shear | `slug_fill.wgsl:393` | **open / known limitation** (§3) |
| D4 | Moderate | band binning over-includes curves; no per-band sort | `GeodePathEncoder.cc:253` | **open** (perf only; correctness benign) |
| D5 | Moderate | even-odd fill rule | `slug_fill.wgsl` | **resolved/moot** — integer parity `(winding & 1)`, never a fractional fold |
| D6 | Moderate | absolute degeneracy threshold + missing `b≈0` NaN guard | `slug_fill.wgsl:233` | **open / known limitation** (§3) |
| D7 | — | (`0x2E74` classification) | — | **moot** — not present on the clean tree; committed shaders use one-directional `curve_winding`, no classification tracer (a mid-edit-artifact reading, like D5) |
| D8 | Benign | orthographic-2D half-pixel dilation | `slug_fill.wgsl:182` | no action — correct for affine 2D |
| D9 | Benign | lines as degenerate quads (control pt = midpoint) | `GeodePathEncoder.cc:147` | no action — standard Slug |
| D10 | Benign | implicit-close of open subpaths | `GeodePathEncoder.cc:116` | no action — SVG-fill conformance |
| D11 | Benign | ~32px/band, max 256 | `GeodePathEncoder.cc:37` | no action — impl-defined granularity |
| D12 | Benign | f32 SSBO curve storage vs reference f16 textures | `slug_fill.wgsl:91` | no action — more precise WebGPU adaptation |

### Implementation notes (resolved Critical/Moderate items)

**D1 — `solve_quadratic` stability.** All six shaders previously used the naive
`(-b ± √disc)/2a`, which loses precision to subtractive cancellation when `b` and a root
share a sign — worst exactly at the tangent/grazing crossings the winding count is most
sensitive to. Replaced uniformly with the Citardauq form (§1.2 step 3). Output-neutral on
every passing test (strict-identity geode goldens, all three resvg parity modes, geode
unit/encoder/shader tests, CPU lanes unchanged); a robustness improvement for grazing
crossings, flips no gates.

**D2 — cubic→chord landmine.** `extractCurves`'s `CurveTo` case used to discard both
cubic control points and emit a quad with control point = endpoint midpoint = a straight
chord — silently wrong geometry. Dead in current call paths (encode runs
`cubicToQuadratic` first), but a future caller handing a raw cubic in would have silently
flattened every cubic. Replaced with `UTILS_RELEASE_ASSERT_MSG(false, …)` documenting the
cubic-free invariant — hard-fail instead of silent corruption. Unreachable today, so
output-neutral.

**D5 — even-odd fill rule.** An earlier audit recorded a triangle-wave fold of
*fractional* coverage for even-odd, but that was read mid-edit during the reverted
analytical-AA experiment ([0039](0039-geode_analytical_aa.md)). The committed
`slug_fill.wgsl` has no `apply_fill_rule` and no fractional fold: even-odd is integer
crossing parity (`(winding & 1) != 0`) in `sample_is_inside`, evaluated per sub-pixel
sample on the 4× MSAA `sample_mask` path — identical across all shaders. Geode has a
single, spec-correct even-odd behavior. **Invariant:** if the analytical-AA coverage
rework is ever revisited, it must preserve integer-parity even-odd.

---

## 3. Known limitations / future work

These are genuine, still-open divergences from the published method. They are scoped to
the analytical-AA coverage rework (which is shelved — [0039](0039-geode_analytical_aa.md)),
because they only change observable output once a fractional-coverage tracer lands; on the
current integer-winding `sample_mask` path they do not affect the accepted edge floor.

- **D3 — per-pixel scale ignores rotation/shear** (`slug_fill.wgsl:393`).
  `m = 1/abs(dpdx(sample_pos).x)` (and `.y`) takes only the axis-aligned derivative
  component; the reference uses the full gradient magnitude (`1/fwidth` ≈
  `1/length(dpdx)`). Since `abs(dx.x) ≤ length(dx)`, `m` is overestimated → coverage ramp
  too steep, worst on thin/rotated features (glyph stems). **Intended fix:**
  `m = 1/max(length(dpdx(sample_pos)), 1e-8)` per ray. Only matters once fractional
  coverage uses `m`.

- **D4 — band binning over-includes curves; no per-band sort**
  (`GeodePathEncoder.cc:253`). Binning by control-point AABB over-includes curves into
  bands, and curves are unsorted within a band. **Correctness-benign** (extra curves just
  contribute zero winding for a ray that misses them); a **perf** opportunity — tighter
  binning by true curve extent + per-band sort. Open, optional.

- **D6 — absolute degeneracy threshold + missing NaN guard** (`slug_fill.wgsl:233`).
  `abs(a) < 1e-4` is in raw path-space units, so genuine shallow quads at large
  coordinates (or near-degenerate lines at tiny coordinates) misfire the linear fallback;
  and a quad with both `a≈0` and `b≈0` divides by ~0 → NaN coverage. **Intended fix:**
  make the threshold scale-relative (vs control-point span / `max(|a|,|b|,|c|)`) and guard
  the linear fallback against `b≈0`. Only observable once fractional coverage lands.

---

## Appendix — audit history

The original ledger (opened 2026-05-25) was an investigation tracker. Two entries (D1's
"fourth analytical tracer" framing, D5's triangle-wave fold) were **mid-edit artifacts** —
the audit read `slug_fill.wgsl` while the [0039](0039-geode_analytical_aa.md) analytical-AA
experiment was in flight (since reverted). On the clean committed tree all six shaders run
the same integer-winding tracer and the same `solve_quadratic`; there is **no `0x2E74`
analytical-classification tracer anywhere** (that remains 0039 future work), and even-odd
is integer parity everywhere. D1 (real shared item: cancellation-prone `solve_quadratic`)
and D2 were then closed as hygiene fixes; D5 was re-grounded as moot; D3/D4/D6 carry
forward as §3.
