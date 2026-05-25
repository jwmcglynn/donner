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
| D1 | Critical | cancellation-prone `solve_quadratic` `(-b ± √disc)/2a` in all six Slug shaders | `slug_fill.wgsl`, `slug_gradient.wgsl`, `slug_mask.wgsl` + the 3 `*_alpha_coverage.wgsl` | **RESOLVED 2026-05-25** — stabilized (see "D1 re-grounded" below). The "fourth analytical tracer" framing was a mid-edit artifact: on the clean tree all six shaders run the *same* integer-winding `curve_winding`, so there is no tracer divergence; the real item was the shared cancellation-prone `solve_quadratic`, now replaced with the numerically-stable Citardauq form uniformly (output-neutral) |
| D2 | Critical (latent) | encoder `CurveTo` silently collapses a cubic to a straight chord (midpoint control pt) instead of subdividing | `GeodePathEncoder.cc:184` | **RESOLVED 2026-05-25** — replaced the silent chord-flatten with a `UTILS_RELEASE_ASSERT_MSG` documenting the cubic-free invariant (`encode` runs `cubicToQuadratic` first, so the case is unreachable; hard-fail rather than silently produce wrong geometry) |
| D3 | Critical | pixels-per-em scale `m` uses `abs(dpdx().x)`/`abs(dpdy().y)`, not `length(dpdx())`/`fwidth` — overestimates `m` under rotation/sub-pixel features | `slug_fill.wgsl:439` | **in flight** (0039 AA increment) |
| D4 | Moderate | band binning by control-point AABB over-includes curves; no per-band sort | `GeodePathEncoder.cc:253` | open (perf; benign correctness) |
| D5 | Moderate | even-odd implemented as a triangle-wave fold of *fractional* coverage, not integer crossing parity | `slug_fill.wgsl:340` | **MOOT / RESOLVED 2026-05-25** — re-grounded against the clean tree: the triangle-wave fold was part of the reverted analytical-AA experiment. The committed `slug_fill.wgsl` uses integer-parity even-odd (`(winding & 1) != 0`) via `sample_is_inside` on the MSAA `sample_mask` path; no `apply_fill_rule`/fractional fold exists. No code change |
| D6 | Moderate | degeneracy threshold `abs(a)<1e-4` is absolute (path-space units), not scale-relative; no `b≈0` NaN guard on the linear fallback | `slug_fill.wgsl:236`, `:319` | **in flight** (0039 AA increment) |
| D7 | Benign | classification index uses `0x2E74>>idx` shift vs reference sign-bit form — numerically equivalent (verified all 8 sign configs); only differs at the measure-zero `y==0` boundary | `slug_fill.wgsl:304` | no action |
| D8 | Benign | half-pixel dilation uses the orthographic-2D simplification, not the full perspective quadratic — correct for Geode's affine 2D content | `slug_fill.wgsl:182` | no action |
| D9 | Benign | lines encoded as degenerate quads (control pt = midpoint) — the standard Slug approach | `GeodePathEncoder.cc:147` | no action |
| D10 | Benign | implicit-close of open subpaths — correct SVG-fill conformance adaptation (Slug assumes closed contours) | `GeodePathEncoder.cc:116` | no action |
| D11 | Benign | ~32px/band, max 256 — implementation-defined banding granularity | `GeodePathEncoder.cc:37` | no action |
| D12 | Benign | f32 SSBO curve storage vs the reference's f16 textures — more precise, legitimate WebGPU adaptation | `slug_fill.wgsl:91` | no action |

## The real divergences (detail)

### D1 — re-grounded + RESOLVED (cancellation-prone `solve_quadratic`)
The original entry ("three of four pipelines run a non-Slug winding algorithm; only `slug_fill.wgsl`
was moved to the analytical tracer") was a **mid-edit artifact** — the audit read the files while the
[0039](0039-geode_analytical_aa.md) analytical-AA experiment was in flight in `slug_fill.wgsl` (since
reverted). On the **clean committed tree**, all six Slug shaders (`slug_fill`, `slug_gradient`,
`slug_mask`, and the three vendor-gated `*_alpha_coverage` variants) run the **same** integer-winding
`curve_winding` tracer (one-directional scanline count with `dy/dt`-sign crossing accumulation) — so
there is no fill-vs-others tracer divergence to converge. There is **no `0x2E74` analytical
classification tracer anywhere**; that remains future work tracked in 0039.

The real, shared item was the **cancellation-prone `solve_quadratic`** — all six used the byte-
identical naive `(-b ± √disc) / 2a`, which loses precision to subtractive cancellation when `b` and a
root share a sign (worst exactly at the tangent/grazing crossings the winding count is most sensitive
to). **Closed 2026-05-25:** replaced uniformly across all six shaders with the numerically-stable
Citardauq form (`q = -(b + sign(b)·√disc)/2`, `t0 = q/a`, `t1 = c/q`, with a `q≈0` double-root
fallback) — matching the Slug reference's robust root extraction. **Output-neutral** on every passing
test: the strict-identity geode golden suite, all three resvg parity modes, the geode unit/encoder/
shader tests, and the CPU lanes were unchanged (verified before/after). Robustness improvement for
grazing crossings; it flips no gates (the remaining gates are the accepted-by-design edge floor).

### D2 — cubic CurveTo collapses to a chord (latent landmine) — RESOLVED
`extractCurves`'s `CurveTo` case discarded both cubic control points and emitted a quad with control
point = endpoint midpoint = a straight line. Dead in current call paths (`GeodePathEncoder::encode`
runs `cubicToQuadratic` first, so `extractCurves` only ever sees QuadTo segments), but any future
caller handing a raw cubic in would have silently flattened every cubic with no error. **Closed
2026-05-25:** replaced the silent chord-flatten with `UTILS_RELEASE_ASSERT_MSG(false, …)` documenting
the "encode requires cubic-free input" invariant — hard-fail instead of silently producing wrong
geometry (no-dead-code discipline). Unreachable today, so output-neutral; the encoder/golden/parity
suites are unchanged.

### D3 — per-pixel scale ignores rotation/shear
`m = 1/abs(dpdx(sample_pos).x)` (and `.y`) takes only the axis-aligned derivative component;
the reference uses the full gradient magnitude (`1/fwidth` ≈ `1/length(dpdx)`). Since
`abs(dx.x) ≤ length(dx)`, `m` is overestimated → coverage ramp too steep, worst on thin/rotated
features (glyph stems). Independently matches the AA increment's observed text-glyph regression.
**Close (in flight):** `m = 1/max(length(dpdx(sample_pos)), 1e-8)` per ray.

### D5 — MOOT (no triangle-wave fold on the clean tree)
The audit recorded an `apply_fill_rule` triangle-wave fold of *fractional* coverage for even-odd —
but that was part of the reverted [0039](0039-geode_analytical_aa.md) analytical-AA experiment, read
mid-edit. The **committed `slug_fill.wgsl`** has no `apply_fill_rule` and no fractional fold: even-odd
is integer crossing parity (`(winding & 1) != 0`) in `sample_is_inside`, evaluated per sub-pixel
sample on the 4× MSAA `sample_mask` path — identical to the other shaders. Geode has a single,
spec-correct even-odd behavior. **No code change; resolved by re-grounding.** (If the analytical-AA
coverage rework lands later, it must preserve integer-parity even-odd — re-open then if it doesn't.)

### D6 — absolute degeneracy threshold + missing NaN guard
`abs(a) < 1e-4` is in raw path-space units, so genuine shallow quads at large coordinates (or
near-degenerate lines at tiny coordinates) misfire the linear fallback; and a quad with both
`a≈0` and `b≈0` divides by ~0 → NaN coverage (the AA increment's text-glyph garbage). **Close
(in flight):** make the threshold scale-relative (vs control-point span / `max(|a|,|b|,|c|)`)
and guard the linear fallback against `b≈0`.

## Sequencing

Status as of 2026-05-25 (re-grounded against the clean committed tree after the 0039 analytical-AA
experiment was reverted):

- **D1** (cancellation-prone `solve_quadratic`) — ✅ RESOLVED: stabilized uniformly across all six
  shaders (output-neutral). The "analytical-tracer convergence" framing was a mid-edit artifact; all
  shaders already share the integer-winding tracer, so nothing to converge. A future `0x2E74`
  analytical classification tracer remains 0039 work.
- **D5** (triangle-wave even-odd) — ✅ MOOT: the fractional fold never existed on the clean tree;
  committed even-odd is integer parity. No code change.
- **D2** (cubic→chord landmine) — ✅ RESOLVED: hard-fail assert documenting the cubic-free invariant.
- **D3, D6** (per-pixel scale / degeneracy threshold + NaN guard) — still tracked in
  [0039](0039-geode_analytical_aa.md) (they belong to the analytical-AA coverage rework, not this
  hygiene pass; they only matter once the fractional-coverage tracer lands).
- **D4** — optional perf: tighter band binning by true curve extent. Open.
- **D7–D12** — benign, no action.

Each close lands in-place, green, with the geode-vs-tiny parity suite + the strict-identity geode
golden suite as the output-neutrality gate; un-gate edge-floor entries only when they measure ≤100 px.
