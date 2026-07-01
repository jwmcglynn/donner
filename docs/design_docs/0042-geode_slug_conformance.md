# 0042 — Geode Slug implementation: developer reference

**Status:** Developer reference. Describes Geode's as-built implementation of the
**Slug algorithm** (per-pixel ray/Bézier-crossing winding) — the pipeline a contributor
must understand to work on the fill/coverage shaders or the path encoder — plus the
invariants it relies on and the known limitations (D3/D4/D6) that remain future work.
The per-pixel **AA coverage** math is documented in [0041](0041-geode_analytical_aa.md);
this doc covers the rest of the pipeline.

**Related:** [0041 anti-aliasing](0041-geode_analytical_aa.md),
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

Per pixel, `fs_main` looks up its horizontal and vertical band in O(1) from a dense grid
and casts **two** rays (the analytic dual-ray scheme from
[0041](0041-geode_analytical_aa.md), landed):

1. **Per-pixel scale:** `ppem = 1.0 / fwidth(sample_pos)`, computed per axis
   (`slug_fill.wgsl:362`) — the fix for the former dpdx/dpdy-only scale (previously
   tracked as D3, now resolved).
2. **Band lookup:** `hBandGrid`/`vBandGrid` map a dense grid cell (by `sample_pos.y` /
   `sample_pos.x`) to a slot in `bands`/`vBands`, or the `kNoBand` sentinel
   (`slug_fill.wgsl:364-383`). One bounding quad covers the whole path, so each pixel is
   rasterized by exactly one fragment invocation (no band-seam double-counting).
3. **Root-find per curve, per ray:** `accumulateHoriz`/`accumulateVert`
   (`slug_fill.wgsl:254-323`) form the quadratic in the ray parameter and call
   `solve_quadratic`, which uses the **numerically-stable Citardauq form** (`q = -(b +
   sign(b)·√disc)/2`, `t0 = q/a`, `t1 = c/q`, with a `q≈0` double-root fallback).
4. **Per-root coverage + weight:** each valid root (`t ≥ 0`) contributes a signed pixel
   distance `r = (crossing − sample) * ppem`, accumulating `cov += ±saturate(r + 0.5)`
   and `wgt = max(wgt, saturate(1 - 2|r|))` — Slug's analytic per-root coverage/weight
   formula (§1 in [0041](0041-geode_analytical_aa.md)).
5. **Combine:** `calc_coverage` (`slug_fill.wgsl:329-333`) blends the two rays'
   `(cov, wgt)` pairs, with a `min(|hCov|, |vCov|)` floor for near-axis-aligned/thin
   features (the crosshair case in [0041 §6](0041-geode_analytical_aa.md#6-blocker-a--the-crosshair--why-the-parity-gate-must-change-not-the-math)).
6. **Fill rule:** non-zero saturates the combined coverage; even-odd folds the
   **unsaturated** combined coverage through a triangle wave (`slug_fill.wgsl:387-394`)
   — a genuine fractional fold, not integer parity.
7. **Coverage → color:** the pipeline runs at `sampleCount = 1` (no MSAA, no
   `sample_mask`); the resulting fractional `coverage` scales the premultiplied paint
   color directly.

`slug_fill`, `slug_gradient`, and `slug_mask` all run this same dual-ray accumulation
and `solve_quadratic` core. The vendor-gated `*_alpha_coverage` shader variants and the
4× MSAA `sample_mask` path described in earlier revisions of this doc are deleted — see
[0041](0041-geode_analytical_aa.md) for the migration.

---

## 2. Conformance ledger

Severity: **Critical** = correctness/parity bug, broad blast radius; **Moderate** =
narrower or conditional; **Benign** = legitimate adaptation or already-correct. Most are
resolved (implementation notes / invariants in §1); the open ones are the known
limitations in §3.

| # | Sev | Area | Geode location | Status |
|---|-----|------|----------------|--------|
| D1 | Critical | `solve_quadratic` numerical stability | `slug_fill.wgsl`, `slug_gradient.wgsl`, `slug_mask.wgsl` | **resolved** — Citardauq form (§1.2 step 3) |
| D2 | Critical (latent) | encoder must not silently flatten cubics | `GeodePathEncoder.cc:185` | **resolved** — hard-fail assert documents the cubic-free invariant (§1.1) |
| D3 | Critical | pixels-per-em scale ignores rotation/shear | `slug_fill.wgsl:362` | **resolved** — `ppem = 1/fwidth(sample_pos)` (§1.2 step 1), landed with the [0041](0041-geode_analytical_aa.md) dual-ray rewrite |
| D4 | Moderate | band binning over-includes curves; no per-band sort | `GeodePathEncoder.cc` | **open** (perf only; correctness benign) — see §3 |
| D5 | Moderate | even-odd fill rule | `slug_fill.wgsl:387-394` | **resolved, and now a real fractional fold** — even-odd folds the unsaturated dual-ray coverage through a triangle wave (§1.2 step 6); no longer integer parity |
| D6 | Moderate | absolute degeneracy threshold + missing `b≈0` NaN guard | `slug_fill.wgsl:204` | **open / known limitation** (§3) — now user-visible since coverage is fractional |
| D7 | — | (`0x2E74` classification) | — | **moot** — Geode's dual-ray shader (§1.2) uses direct per-root sign-of-tangent classification, not the reference's `0x2E74` lookup table; algorithmically equivalent, different implementation |
| D8 | Benign | orthographic-2D half-pixel dilation | `slug_fill.wgsl` vertex stage | no action — correct for affine 2D |
| D9 | Benign | lines as degenerate quads (control pt = midpoint) | `GeodePathEncoder.cc:147` | no action — standard Slug |
| D10 | Benign | implicit-close of open subpaths | `GeodePathEncoder.cc:116` | no action — SVG-fill conformance |
| D11 | Benign | ~32px/band, max 256 | `GeodePathEncoder.cc:37` | no action — impl-defined granularity |
| D12 | Benign | f32 SSBO curve storage vs reference f16 textures | `slug_fill.wgsl` | no action — more precise WebGPU adaptation |

### Implementation notes (resolved Critical/Moderate items)

**D1 — `solve_quadratic` stability.** The shaders previously used the naive
`(-b ± √disc)/2a`, which loses precision to subtractive cancellation when `b` and a root
share a sign — worst exactly at the tangent/grazing crossings the winding count is most
sensitive to. Replaced uniformly with the Citardauq form (§1.2 step 3). A robustness
improvement for grazing crossings.

**D2 — cubic→chord landmine.** `extractCurves`'s `CurveTo` case used to discard both
cubic control points and emit a quad with control point = endpoint midpoint = a straight
chord — silently wrong geometry. Dead in current call paths (encode runs
`cubicToQuadratic` first), but a future caller handing a raw cubic in would have silently
flattened every cubic. Replaced with `UTILS_RELEASE_ASSERT_MSG(false, …)` documenting the
cubic-free invariant — hard-fail instead of silent corruption. Unreachable today, so
output-neutral.

**D3/D5 — the analytical-AA rework landed.** Earlier revisions of this doc described D3
(per-pixel scale) and D5 (fractional even-odd fold) as open/moot future work gated behind
a "shelved" analytical-AA rework. That rework is [0041](0041-geode_analytical_aa.md),
which has since landed: `slug_fill.wgsl` now computes `ppem` via `fwidth` (D3's intended
fix) and folds even-odd coverage through a genuine triangle wave (D5). See §1.2 for the
as-built pipeline.

---

## 3. Known limitations / future work

These are genuine, still-open divergences from the published method, on the current
dual-ray analytic pipeline ([0041](0041-geode_analytical_aa.md), landed).

- **D4 — band binning over-includes curves; no per-band sort**
  (`GeodePathEncoder.cc`). Binning by control-point AABB over-includes curves into bands,
  and curves are unsorted within a band. **Correctness-benign** (extra curves just
  contribute zero coverage for a ray that misses them); a **perf** opportunity — tighter
  binning by true curve extent + per-band sort. Open, optional.

- **D6 — absolute degeneracy threshold + missing NaN guard** (`slug_fill.wgsl:204`).
  `abs(a) < 1e-4` is in raw path-space units, so genuine shallow quads at large
  coordinates (or near-degenerate lines at tiny coordinates) misfire the linear fallback;
  and a quad with both `a≈0` and `b≈0` divides by ~0 → NaN coverage. **Intended fix:**
  make the threshold scale-relative (vs control-point span / `max(|a|,|b|,|c|)`) and guard
  the linear fallback against `b≈0`. Now directly observable (fractional coverage is
  live), not just theoretical — open.

