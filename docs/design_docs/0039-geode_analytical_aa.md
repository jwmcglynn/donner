# 0039 — Geode analytical (Slug-style) edge coverage

**Status:** proposal + validated POC (opened 2026-05-25). Investigates Slug's
analytical antialiasing, documents the exact gap vs Geode's current 4× MSAA +
existing alpha-coverage shaders, and proposes an incremental plan to give Geode
smooth (un-quantized) edge coverage so the **191 edge-floor parity gates** (0017
§Phase 4b / 0038) reach ≤100 px geode-vs-tiny and un-gate in batches.
**Author:** GeodeBot (investigation), Jeff McGlynn (review)
**Created:** 2026-05-25
**Related:** [0017 §Phase 4b](0017-geode_renderer.md#phase-4b-in-process-backend-matrix--geode-vs-tiny-skia-parity-comparison),
[0038 text parity](0038-geode_tinyskia_text_parity.md),
[0021 §G5](0021-resvg_feature_gaps.md#g5-audit-the-aa-justified-geode-thresholds)

---

## 1. The problem: a 64-step edge floor, not anti-aliasing

The geode-vs-tiny-skia parity suite has **191 edge-floor gates**: tests where
Geode renders the *correct* shapes/glyphs/colors, but the geode↔tiny diff sits
at **101–763 px** — just over the flat-100 parity bar. The audit (0038, 0017
§Phase 4b) proved the residual is a **thin band hugging shape edges** whose
**alpha is quantized to 5 levels: 0, 64, 128, 191, 255** (i.e. 0, 1/4, 2/4, 3/4,
4/4). pixelmatch already runs with `includeAA=false`, so this is *not* an "it's
just AA" hand-wave — it is a concrete, measurable **coverage quantization** that
differs from tiny-skia's ~256-level supersampled coverage at every partially
covered edge pixel.

This is per the CLAUDE.md rule: AA is not the root cause; the **root cause is
that Geode computes edge coverage from exactly 4 sub-pixel samples per pixel**.
The fix is *analytical* coverage (the Slug method), not more MSAA samples.

---

## 2. What Slug's analytical antialiasing actually is

Source: Eric Lengyel, **"GPU-Centered Font Rendering Directly from Glyph
Outlines," JCGT vol. 6 no. 2 (2017)**
([jcgt.org/published/0006/02/02](https://jcgt.org/published/0006/02/02/)), the
["A Decade of Slug" retrospective](https://terathon.com/blog/decade-slug.html),
and the author-endorsed reference implementation
[mightycow/Sluggish](https://github.com/mightycow/Sluggish) (`code/renderer_gl/main.cpp`,
the GLSL fragment shader). The patent (US10373352B1) was dedicated to the public
domain on 2026-03-17; reference shaders are MIT. Geode implements the algorithm
clean-room (ISC) — these are algorithmic references only.

### 2.1 Core idea — coverage from crossing distance, not sample counting

Slug casts a **horizontal ray** from the pixel center and finds where each
Y-monotonic Bézier curve crosses it (the same root-finding Geode already does).
Instead of accumulating an **integer winding number** (inside/outside → a hard
black-and-white edge), it accumulates a **fractional coverage** that reflects how
far each crossing is from the pixel center. The paper's **Equation 3**:

> *f* = sat( *m* · C<sub>x</sub>(*t*<sub>i</sub>) + ½ )

where **C<sub>x</sub>(t<sub>i</sub>)** is the x-coordinate of the crossing
**relative to the pixel center** (in em/path space), **m** is the number of
pixels per em (the path-space→pixel scale along the ray), and **sat** clamps to
[0, 1]. The fraction is **added** for one crossing direction and **subtracted**
for the other (matching the winding sign). The fill's alpha is
`min(abs(Σf), 1)` for non-zero fill.

Geometrically: a crossing exactly at the pixel center (C<sub>x</sub>=0) gives
f=½ (half the pixel covered); a crossing half a pixel to the right (C<sub>x</sub>
= +0.5/m) gives f=1; half a pixel to the left gives f=0. Coverage is a
**continuous ramp** across the one-pixel band straddling the edge — exactly the
analytical coverage of a straight edge integrated along the scanline. **No
supersampling, one ray per pixel.**

The reference shader (Sluggish `TraceRayCurveH`) is literally:

```glsl
float c = clamp(x1 * pixelsPerEm + 0.5, 0.0, 1.0);   // Eq. 3, f for root t1
coverage += c;                                        // entering crossing
...
float c = clamp(x2 * pixelsPerEm + 0.5, 0.0, 1.0);   // f for root t2
coverage -= c;                                        // leaving crossing
```

with `pixelsPerEm = 1.0 / fwidth(texCoords)` (derivative-based, per-pixel).

### 2.2 The second ray — conflation reduction

A single horizontal ray "antialiases in the direction of the ray" (paper §2.4)
— i.e. it smooths **near-vertical** edges perfectly, but leaves a
**near-horizontal** edge as a hard step (the ray never measures sub-pixel
distance in y). To fix this conflation, Slug **also casts a vertical ray** (the
same math with x/y swizzled) and **averages the two coverages**:

```glsl
coverageX = min(abs(coverageX), 1.0);     // horizontal ray
coverageY = min(abs(coverageY), 1.0);     // vertical ray
alpha = (coverageX + coverageY) * 0.5;    // isotropic analytical coverage
```

The paper: *"Averaging the final coverages calculated for multiple ray
directions antialiases with greater isotropy… Considering only rays parallel to
the coordinate axes is a good compromise."* The vertical ray requires a
**separate set of vertical bands** (curves sorted/indexed by x), because the
curves intersecting a horizontal band are *not* the curves a vertical ray at an
arbitrary x would hit. This is structural, not optional — see §4.

### 2.3 What Slug does NOT do

- **No supersampling.** Adaptive supersampling existed in early Slug and was
  *removed* — "it made a difference only for text so small it was barely
  readable, and dilation mitigated tiny-text aliasing." (0017 already records
  this.) Slug's production AA is **1 sample/pixel, two analytical rays.**
- **No MSAA / `sample_mask`.** Coverage is a fragment-shader scalar folded into
  alpha; the render target is single-sample.
- **No distance field / atlas.** Curves are evaluated per-pixel.

---

## 3. How Geode's current AA compares — the exact gap

Geode has **two** coverage paths, both in `donner/svg/renderer/geode/shaders/`,
and **neither is analytical**:

### 3.1 Default path — 4× MSAA + `@builtin(sample_mask)` (`slug_fill.wgsl` etc.)

The fragment shader (`fs_main`) evaluates the **integer winding** test
(`sample_is_inside`) at **4 fixed sub-pixel offsets** (a D3D rotated-grid), packs
the 4 inside/outside bits into `@builtin(sample_mask)`, and writes one solid
color. The 4× MSAA hardware resolve then averages the surviving samples:

```wgsl
for (var s: u32 = 0u; s < 4u; s = s + 1u) {
  let sp = in.sample_pos + offsets[s].x*dx + offsets[s].y*dy;
  if (sample_is_inside(band, sp)) { mask = mask | (1u << s); }
}
```

→ the resolved alpha is `popcount(mask)/4` ∈ **{0, ¼, ½, ¾, 1}** = the
**5-level / 64-step quantization** the parity suite measures. This is **box
supersampling at 4 samples**, *not* analytical coverage. tiny-skia's
scan-converter integrates coverage at ~256 levels; the gap at every edge pixel is
the |4-level − 256-level| difference, which sums to 101–763 px over a glyph/shape
perimeter. **This is the active path on CI's Mesa llvmpipe and on most adapters
(Metal, etc.).**

### 3.2 Fallback path — alpha-coverage (`*_alpha_coverage.wgsl`)

The Intel-Arc+Vulkan fallback (gated in `GeodeDevice.cc` on
`vendorID==0x8086 && backendType==Vulkan`, `useAlphaCoverageAA_`) avoids the
`sample_mask` write (which hangs Mesa ANV). It runs the **identical 4-sample
loop**, then folds:

```wgsl
let coverage = f32(countOneBits(mask)) / 4.0;   // STILL 5 levels
out.color = uniforms.color * coverage;
```

So the existing "alpha-coverage" shader is **not** Slug analytical coverage — it
is the *same 4-sample quantization* moved from the hardware resolve into the
fragment color. It produces byte-identical 5-level output to the MSAA path. The
name is misleading; it's a *single-sample MSAA emulation*, not analytical AA.

### 3.3 The structure Geode already has vs. what it's missing

| Slug analytical AA needs | Geode has it? |
|---|---|
| Per-pixel Bézier root-finding along a ray | ✅ `solve_quadratic` / `curve_winding` (the x of each crossing is already computed) |
| Pixels-per-em scale `m` along the ray | ✅ derivable from `dpdx(sample_pos)` (already used for sub-pixel offsets) |
| Eq. 3 `sat(m·Cx + ½)` per crossing + signed sum | ❌ — Geode counts integer winding, never the fractional distance |
| Horizontal bands (curves sorted for horizontal ray) | ✅ `EncodedPath.bands` (`yMin/yMax`, Y-monotonic curves) |
| **Vertical** bands (for the second ray) | ❌ — encoder emits horizontal bands only |
| Single-sample target (no MSAA needed) | ⚠️ supported (`sampleCount()==1` on the alpha-coverage path) but not the default |

**The gap is exactly:** (a) replace integer-winding-at-4-samples with Eq. 3
fractional coverage along **one** ray (cheap — uses data already in the shader),
and (b) add **vertical bands** to the encoder + a vertical ray, then average the
two. (a) alone halves the edge floor; (a)+(b) reaches Slug-quality isotropic
coverage.

---

## 4. POC — validated on the default (Metal/MSAA-equivalent) path

I implemented **step (a) only** — single horizontal-ray analytical coverage
(Eq. 3) — directly in `slug_fill.wgsl`'s `fs_main`, keeping the existing 4× MSAA
pipeline by writing `sample_mask = 0xF` (all samples) and folding the analytical
coverage into the premultiplied color. With all 4 MSAA samples carrying the same
analytical alpha, the resolve averages four identical values → **the
quantization is gone** (the alpha is now continuous), with **zero pipeline /
sampleCount / blend-state change** required for the POC.

Measured on the local **Apple M4 Pro / Metal** adapter (which, like CI's
llvmpipe, takes the default MSAA path — `useAlphaCoverageAA_ == false`), via the
`GeodeTinyParity` mode (pixelmatch 0.02, `includeAA=false`), using a temporary
`GEODE_PARITY_MEASURE_ALL=1` escape hatch to read px on the gated tests:

| Test (edge-floor gate) | Before (4× MSAA) | After (1 H-ray analytical) |
|---|---:|---:|
| `text/text/simple-case` | **708** | **295** |
| `text/text/rotate` | 652 | 288 |
| `text/text/nested` | 720 | 398 |
| `text/text-decoration/underline` | 698 | 296 |
| `structure/svg/preserveAspectRatio=xMidYMid` | 482 | 568 |
| `shapes/line/no-y1-coordinate` | 107 | 115 |
| `painting/stroke/control-points-clamping-1` | 146 | 136 |

**Findings:**

1. **Text drops ~55–60%** (708→295, 698→296). The horizontal ray smooths the
   dominant **near-vertical** glyph stems perfectly — the quantization on those
   edges is eliminated. This directly confirms the diagnosis: the edge floor is
   coverage quantization, and Eq. 3 removes it.
2. **No text test reaches ≤100 px on one ray.** The residual is the
   **near-horizontal** edges (tops/bottoms of glyphs, baselines, underlines)
   that a horizontal ray renders as a hard step — exactly the conflation §2.2
   says the **vertical ray** fixes.
3. **`preserveAspectRatio`/`line` got slightly *worse*** (482→568, 107→115):
   these scenes are dominated by near-horizontal edges, so swapping a 4-sample
   box estimate (which has *some* y-resolution) for a horizontal-only analytical
   ramp (which has *none* in y) regresses them. **This is the strongest evidence
   that the second ray is mandatory**, not a nice-to-have.
4. **I tried faking the vertical ray** by reusing the horizontal-band curves
   (swizzled x/y). It **blew up to ~45 000 px** — because a vertical ray at a
   given x is not served by the horizontal band's curve list (it misses curves
   outside the band's y-window and double-counts others). **Confirmed: the
   vertical ray genuinely requires vertical bands from the encoder.** The faked
   path was removed from the POC.

**Conclusion:** the Slug method is the right fix and the formula is correct
(text edge floor more than halved with a one-line-per-crossing change), but
reaching ≤100 px requires the **dual-ray + vertical-band** work. Single-ray
alone is a real improvement for text but not sufficient to un-gate, and it
regresses horizontal-edge-dominated scenes — so it must **not** land alone.

> POC scaffolding (to be reverted before any rollout PR): a temporary
> `GEODE_PARITY_MEASURE_ALL` escape hatch in `resvg_test_suite.cc::geodeParityGate`
> (returns `nullopt` so gated tests still emit `PARITY|<file>|<px>`), and the
> single-ray `band_coverage_h` in `slug_fill.wgsl`. No gate was un-gated; no
> threshold was changed.

---

## 5. Performance: analytical AA is a WIN, not a tradeoff

0017 chose 4× MSAA *"for performance"*. Analytical coverage at 1 sample is
**both smoother and cheaper**, for three reasons:

1. **No MSAA color attachment.** The default path allocates a **4× multisample
   color texture + a 1-sample resolve target** and runs a hardware resolve every
   pass. Analytical coverage needs **one single-sample target, no resolve** —
   4× less color-attachment memory bandwidth, which is the dominant cost on a
   bandwidth-bound GPU rasterizer (and on llvmpipe, MSAA resolve is pure CPU
   work).
2. **Fewer root solves per pixel.** The MSAA path runs `sample_is_inside` **4×
   per pixel** (4 sub-pixel offsets, each looping all band curves →
   `4 × curveCount` quadratic solves). One analytical ray is **1× per pixel**
   (`1 × curveCount`). The full dual-ray is **2× per pixel** — still **half** the
   per-pixel curve work of the 4-sample path, while giving ~256-level coverage
   instead of 5-level. (This matches the Slug paper's whole thesis: 1–2
   analytical rays beat supersampling on both quality and cost.)
3. **Simpler shader, less divergence.** No per-sample mask packing; the inner
   loop is a straight `coverage += clamp(...)` with no branches (the paper notes
   "no branching is necessary to calculate a coverage value").

The only added cost is **CPU-side vertical-band encoding** (§6 step 2), a
one-time per-path preprocessing step amortized by the existing
`GeodePathCacheComponent` (planned Phase 5 path cache). Net: **clear win** —
this is why it should replace, not coexist with, the MSAA path.

> A perf gate should accompany the rollout: a `donner_perf_cc_test` measuring
> per-frame GPU time on `donner_splash.svg` + a text-heavy doc, asserting the
> analytical path is **not slower** than the current MSAA path (expected
> faster). Consult PerfBot for the harness.

---

## 6. Incremental implementation plan (in-place, each step green)

The 191 edge-floor gates are the verification: each step measures the gated px
and un-gates the batch that crosses ≤100. **No gate un-gates until it genuinely
measures ≤100 px** (no per-test thresholds, no MSAA sample bump, no masking —
0017 / 0038 policy). tiny-skia stays byte-identical throughout (geode-only
changes).

1. **Eq. 3 horizontal-ray coverage in `slug_fill.wgsl` (no pipeline change).**
   Land `curve_coverage_h` + `band_coverage_h` + the `fs_main` rewrite (the POC's
   correct half), still on the 4× MSAA pipeline with `sample_mask = 0xF`.
   **Verify:** `renderer_geode_golden_tests` stays green (strict-0 — the goldens
   are geode-authored; regenerate them in this step since coverage values
   legitimately change), and the text edge-floor px **drops** (no regression
   beyond the documented horizontal-edge cases). **Flips 0 gates** (text still
   >100) — but it's the foundation and is independently correct + faster-ready.
   *Risk to watch:* horizontal-edge-dominated scenes (preserveAspectRatio/line)
   regress; they are already gated, so no gate flips red, but record the new px.

2. **Vertical bands in `GeodePathEncoder`.** Add an `xMonotonic` split + a second
   band list sorted by x (the encoder already does the Y-monotonic split; mirror
   it for X). Extend `EncodedPath` with `vBands` + the curves indexed for the
   vertical ray, and the GPU buffers/bind group to carry them. **Verify:** encoder
   unit tests (`GeodePathEncoder_tests`) for the new band list; no shader change
   yet, so golden output is unchanged (the new buffer is unused) — proves the
   encoding lands inert.

3. **Dual-ray averaging in `slug_fill.wgsl`.** Add `curve_coverage_v` /
   `band_coverage_v` reading the vertical bands, and
   `alpha = (covH + covV) * 0.5`. **Verify:** this is the step that flips the
   **shape + text edge-floor gates** — measure each, un-gate every test that
   crosses ≤100 (batch-drop from `kEdgeFloor` in `resvg_test_suite.cc`). Expect
   the bulk of the 191 here.

4. **Drop MSAA from the fill pipeline.** Switch `GeodeFillPipeline` to
   `sampleCount = 1`, remove the MSAA color attachment + resolve for the fill
   pass, set the standard premultiplied blend. **Verify:** golden + parity stay
   green; capture the perf win (§5 gate). This is the in-place removal of the old
   path (CLAUDE.md: no parallel path — the analytical path *becomes* the path).

5. **Roll the same change through `slug_gradient.wgsl` + `slug_mask.wgsl`.** Both
   share the identical 4-sample machinery (the gradient shader's own comment says
   the coverage code is "mechanically copy-able from slug_fill.wgsl"). Apply
   `band_coverage_h/v` to each; un-gate the gradient-on-text / mask edge-floor
   batch. Covers: **fill, gradient, mask, and text** (text routes through the
   fill/gradient path via `drawText` → `drawPath`).

6. **Delete the `*_alpha_coverage.wgsl` variants + `useAlphaCoverageAA_`** *iff*
   the analytical single-sample path also clears the Intel-Arc+Vulkan
   `sample_mask` hang (it should — analytical coverage writes no `sample_mask`).
   This collapses the two-shader-per-pipeline maintenance into one. Gate on a
   real Intel-Arc run; until then, repoint the alpha-coverage variants at the
   analytical code so they don't diverge. (0017 already wants these deleted once
   Mesa 25.3 lands.)

7. **Perf gate + doc close-out.** Land the `donner_perf_cc_test`; update 0017
   §Phase 4b and 0038 to mark the edge-floor gates retired; record final
   pass/gated numbers.

### Shape-type coverage

All four Slug shaders feed the same shape types:

| Shader | Shapes it covers |
|---|---|
| `slug_fill.wgsl` | solid `drawPath`/`drawRect`/`drawEllipse`, **text glyphs** (fill), text decorations, strokes (`strokeToFill`), patterns |
| `slug_gradient.wgsl` | gradient fills/strokes incl. **gradient-on-text** |
| `slug_mask.wgsl` | clip-path coverage masks, `<mask>` luminance |
| `*_alpha_coverage.wgsl` | Intel-Arc+Vulkan fallback mirror of the above |

So the edge floor is retired for fill, gradient, mask, clip, and text together
once steps 3+5 land.

---

## 7. Feasibility caveats (WebGPU / WGSL / llvmpipe)

- **No fwidth surprise.** Slug uses `fwidth(texCoords)` for `m`; Geode uses
  `dpdx(sample_pos)` (same quantity — path-space delta per viewport pixel).
  `dpdx`/`dpdy` are core WGSL fragment built-ins, fine on llvmpipe and Metal.
- **Premultiplied folding is exact.** Scaling a premultiplied color by a scalar
  coverage stays premultiplied; with `One / OneMinusSrcAlpha` blend (Geode's
  current state) the source-over math is unchanged. Verified in the POC.
- **Even-odd fill rule** needs the signed coverage folded into a triangle wave
  (`1 − |frac(|Σf|/2)·2 − 1|`) rather than `&1`; included in the POC's
  `apply_fill_rule`. Worth a dedicated even-odd test in step 1.
- **Vertical bands cost CPU encode time**, mitigated by the path cache (Phase 5).
  No GPU-side feasibility blocker found.
- **No part of this is infeasible on WGSL/llvmpipe.** The POC ran the analytical
  fragment on llvmpipe's sibling path (Metal default-MSAA path) with no
  validation or compile error. The dual-ray version is the same math twice plus a
  new storage buffer — all standard WGSL.

If a future adapter cannot do single-sample analytical coverage (none found),
the `*_alpha_coverage` slot remains as the per-adapter escape — but pointed at
the analytical code, never back to 4-sample quantization.

---

## 8. Revision (2026-05-25) — dual-ray is a dead end; pivot to §6 supersampling

**Steps 1–3 were implemented twice and reverted twice.** The dual-ray (steps 1–3)
*does* roughly halve the edge floor on real edges (text `simple-case` 708→~290) and is
correct for all non-text geometry (shapes/circles/rects/fill-rule → 0–18 px). But it
**cannot reach ≤100 px vs tiny on the resvg corpus**, for a fundamental reason — not a
tuning or scale (D3) or degeneracy (D6) bug (both were applied, zero effect on the number):

**The conflation problem.** Every resvg text test carries a `stroke-width="0.5"` crosshair
+ 1 px frame (thin axis-aligned lines). A ray cast *along* a sub-pixel-width line never
exits it → reports full coverage (geode 255 vs tiny 160). The two adversarial cases need
**opposite** combine rules:
- thin line aligned with a ray (crosshair) → wants `min` (the perpendicular ray is right);
- genuine perpendicular edge fringe (glyph top) → wants `max` (the aligned ray reads 0).

No single combine satisfies both. Measured on the full 1263-test parity sweep:

| combine | edge-floor flips (≤100) | new regressions (non-gated) |
|---|---|---|
| average (Slug-canonical fast mode) | 1 | **56** (over-covers every thin line) |
| min | ~0 (text stuck ~295) | under-covers perpendicular fringes |
| min/max hybrid | 10 | 16 (skew/transform/letter-spacing) |

Best (hybrid) = +10 / −16 = **net negative**. This is a property of axis-aligned dual-ray,
not a bug to tune out. Plus sub-pixel-phase differences vs tiny's scan-converter accumulate
past 100 over long glyph perimeters.

**Pivot — direction 1: single-axis analytical supersampling (Slug paper §6).** Keep ONE
ray; take N sub-samples shifted *perpendicular* to it (offset ray origin), average. This
matches a scan-converter's coverage *distribution* far better than a second perpendicular
ray and **sidesteps the thin-line conflation entirely** (a horizontal ray measures the
0.5px crosshair width correctly at every vertical sub-position; glyph fringes captured at N
sub-positions). It is the paper's *quality* mode (dual-ray is the *fast* mode). Cost: N× the
single-ray tracer per pixel (no vertical bands needed — big simplification vs steps 2–3).

Discarded alternatives: accept the edge floor (pursue parity via other levers / the MSAA
drop is orthogonal); exclude the harness crosshair from the parity diff (= masking, banned).

**Plan supersedes steps 1–3 above:** prove direction 1 with a small POC (does perpendicular
supersampling get the crosshair AND glyphs ≤100 with no regressions on ~6 representative
tests?) BEFORE a full rollout — two reverts earned a validation gate. Vertical bands
(encoder step 2) are NOT needed for direction 1 and are shelved.

---

## 9. Revision 2 (2026-05-25) — §6 supersampling NO-GO; the premise itself is wrong

The single-axis supersampling POC (one H-ray + N perpendicular sub-samples, no vertical
bands) was built and swept. **NO-GO**, and it reframes the whole effort. Two decisive findings:

### Finding A — tiny-skia is finite-sample (~4×4=16), not analytically smooth
Single-axis supersampling on `text/text/simple-case`: **N=4 → 15px, N=8 → 411px, N=16 → 411px.**
A converging average cannot get *worse* with more samples. So tiny-skia's coverage is **a
specific finite sample pattern** (~16-sample), and N=4 coincidentally aligned with it — not an
analytical limit. **This invalidates the project's premise.** "Analytical coverage" computes
the smooth (∞-sample) limit, which *by construction* differs from tiny's 16-level quantization
by more than the 0.02 parity threshold (1/16 = 0.0625 > 0.02). You cannot match a finite-sample
reference with an analytical method. The real parity target is **tiny's sample count/pattern**,
not Slug's analytical coverage.

### Finding B — folded-alpha coverage doesn't compose across band seams (the N-independent wall)
~168–182 tests regress at *every* N, identically. Root cause (diff-localized on
`structure/use/simple-case`): a pixel straddling a 32px band boundary is shaded by one fragment
per band; each writes coverage ≈0.5 into alpha; premultiplied source-over composes them as
`0.5 + 0.5·(1−0.5) = 0.75` (195) instead of 1.0 (255). The 4× MSAA path is correct because
`@builtin(sample_mask)` writes are **hardware-additive per-sample** (each sample written exactly
once, no double-composition). **Any folded-alpha scheme loses that additivity at band seams** —
this killed dual-ray (vertical-band seams) and supersampling (horizontal-band seams) alike.

### Unifying conclusion
Every analytical-fold attempt (dual-ray, supersampling) hit the same root: **moving coverage
out of `sample_mask` into folded alpha breaks cross-band composition.** The `sample_mask` MSAA
path is correct by construction; folded analytical coverage needs non-overlapping per-pixel band
ownership to be correct, which neither approach provides.

### The reframed options (analytical-AA-to-match-Slug is abandoned)
1. **Raise geode MSAA 4×→16× in the `sample_mask` path.** Keeps hardware-additive composition
   (no seam bug) AND matches tiny's ~16-sample quantization (Finding A) — the actual parity
   target. Cost: ~4× the MSAA rasterization the 0017 perf decision deliberately avoided. Needs
   verification that geode's sample *positions* match tiny's closely enough (the N=4 alignment
   suggests positions matter). This is **not** the "no sample-count-bump shortcut" that was
   earlier banned — that ban assumed tiny was analytical; the data now shows matching tiny's
   sample count IS the correct target.
2. **Accept the edge floor.** The 191 edge-floor gates are cases where geode renders *correctly*;
   the diff is a deliberate 4×-vs-16× sample-count difference. Keep them gated/documented as the
   0017 perf tradeoff; stop chasing pixel-identity on coverage.
3. **Non-overlapping band ownership** (full-height band quads, or single-band encoding for small
   paths) to make folded analytical coverage compose correctly — re-enables the 1-sample perf
   dream, but is the hardest path and still wouldn't match tiny's finite quantization (Finding A),
   so it trades parity for perf, not for parity.

Given Finding A, options 1 and 2 are the only ones that actually reach parity; the analytical
dream (option 3 / the original plan) cannot match a finite-sample reference. Decision pending.

---

## 10. DECISION (2026-05-25): accept the edge floor; analytical AA shelved

After three implement→revert cycles and the two findings in §9, the decision is to **accept
the 191 edge-floor gates as a by-design difference and shelve the analytical-AA effort.**

Rationale: the edge-floor gates are cases where **geode renders correctly** — the only
difference from tiny is the sample count (geode 4× MSAA vs tiny's ~16-sample rasterizer, per
§9 Finding A). Reaching ≤100 px parity would require either 16× MSAA (≈4× the rasterization/
resolve cost, reversing 0017's deliberate 4×-for-perf decision, with no guarantee geode's
sample positions match tiny's) or a from-scratch scan-converter match. Neither is worth the
cost/risk for a *non-bug* sampling difference. Analytical coverage cannot match a finite-sample
reference (§9 Finding A), so the original "match Slug analytical AA" goal is abandoned.

The edge-floor gates remain binary-gated (no per-test thresholds, per policy) but their reason
is reframed from "ratchet out with finer AA" to **"accepted by-design: geode 4× MSAA vs tiny's
~16-sample coverage; geode renders correctly."** If a future need arises (e.g. a print/export
path wanting reference-grade AA), revisit option 1 (16× MSAA, perf-gated) or option 3
(non-overlapping band ownership) from §9 — both recorded there. This doc stays as the record of
why analytical AA was tried and shelved, so it isn't re-attempted.

**Redirected effort:** the genuine remaining geode bugs — `feGaussianBlur/complex-transform`
(CTM projection), [0040](0040-geode_slug_conformance.md) D2 (cubic→chord landmine), and the
re-grounded D1/D5 — which are real correctness gaps, not sampling differences.

---

## 11. DECISION REVISED (2026-05-25): pursue 16× sampling, PerfBot-gated

Superseding §10: pursue matching tiny's ~16-sample coverage (§9 Finding A), **gated on PerfBot
clearing the cost.** Two gates, both must pass before any rollout/un-gate:
1. **Parity gate** — 16× effective sampling actually brings representative edge-floor tests
   (text, shapes, preserveAspectRatio, image, across categories) to ≤100 px geode-vs-tiny.
   Uncertain: geode's sample *positions* must align with tiny's (the N=4-matched-tiny finding
   says positions matter); a 4×4 box grid is the strongest candidate since §9 Finding A points
   to tiny being ~4×4.
2. **Perf gate (PerfBot)** — the per-frame cost vs the current 4× is acceptable for the
   0017 animation frame budget, OR it's scoped so the live animation path keeps 4× (the §9
   option-3 hybrid) while offscreen/one-shot renders get 16×.

**Mechanism feasibility:** WebGPU only guarantees `sampleCount` ∈ {1, 4}; 8×/16× MSAA is
adapter-optional and frequently unsupported (and 16× MSAA resolve has already shown driver
hangs on Intel Arc — see GeodeDevice alpha-coverage fallback). So the likely viable mechanism
is **supersample: render to an N×-larger target and box-downsample** (4×4 → 16 effective
samples). This (a) matches tiny's 4×4 grid distribution, (b) composes correctly (full per-pixel
rendering — no folded-alpha band-seam bug from §9 Finding B), (c) works on every adapter. Cost:
~16× fragment + 16× intermediate memory — exactly what the perf gate must weigh. Check true
MSAA sampleCount support first; fall back to supersample if 16× MSAA is unavailable.

---

## 12. 16× sampling NO-GO (2026-05-25) — the edge floor is sub-perceptual, not sampling

The §11 16×-sampling path was built and measured. **NO-GO for text** (the bulk of the edge
floor), with the most decisive evidence of the whole effort:

**MSAA sampleCount=16 is unsupported** — wgpu-native on Metal (M4 Pro) and llvmpipe both cap at
4 (`"device supports [1,2,4]"`; WebGPU guarantees only {1,4}). So 16× can only be reached by
**supersample** (render to a kSS×-larger target + box-downsample). Implemented: kSS=2 → 16
effective samples, kSS=4 → 64 effective.

**Blocker 1 — text parity is sample-INDEPENDENT (the gate fails for the right reason):**

| test | baseline 4× | 16 eff (kSS=2) | 64 eff (kSS=4) |
|---|--:|--:|--:|
| text/text/simple-case | 708 | 295 | **293** |
| text/text/rotate | 652 | 287 | **287** |
| shapes/circle/simple-case | ~18 | 1 | **0** |
| shapes/line/no-y1-coordinate | 107 | — | **8** |

16 and 64 effective samples give **identical** text px (~290) — quantization would be crushed by
64×, so the residual is **algorithm-level**, proven by diff PNG:
- **glyph edges (~900 px):** geode 96 vs tiny 103 at the 1px AA fringe = **7/255 (2.7%)**, mean
  signed bias ≈ −1 (symmetric, not a positional shift). The inherent geode-coverage-vs-tiny-
  scan-converter delta, sitting *just over* the 0.02 threshold (5.1/255). Sample-independent.
- **horizontal crosshair (~728 px):** the 0.5px reference line — tiny distributes it asymmetrically
  across rows (128/192), geode's symmetric downsample gives 160/160. A geometric line-placement
  difference. (Vertical crosshair is perfect — the H-ray resolves it analytically.)

§9 Finding A held for **shapes** (filled regions → 0–8 px at 16×) but **not for text**: glyph
edges + the thin crosshair are not grid-matchable to ≤100.

**Blocker 2 — full supersample is a pervasive rewrite.** The POC supersamples only the main pass;
offscreen targets (filter/layer/mask/pattern/image) stay 1×, so a full-suite sweep at kSS=4 gave
**0 flips / 200 regressions** (filters at 200k+ px — offscreen content composites at 1× into the
kSS× target). Correctness needs kSS× on every offscreen target + every blit/scissor/clip-rect
coordinate across RendererGeode — large and invasive — and Blocker 1 means it still wouldn't fix
text.

**Conclusion (all four attempts — dual-ray, single-axis SS, MSAA-16, render-SS):** geode's
coverage matches tiny to **~2.7% per edge pixel**; the edge-floor gates exceed 100 px purely from
that sub-perceptual delta accumulating over long perimeters + the harness 0.5px crosshair. **None
of it is fixable by more/better samples.** Perf for the record: kSS=2 ≈ 4× fill cost, kSS=4 ≈ 16×
— moot for text given Blocker 1.

Remaining real options: **(1) accept the edge floor** (it is the characterized, sub-perceptual
geode-vs-tiny rasterizer delta — not a bug, not sampling); **(2)** reimplement geode's coverage to
bit-match tiny's scan-converter (huge, defeats the GPU-native design, uncertain); **(3)**
shapes-only supersample (helps shapes, full offscreen rewrite + 4× perf, text stays gated — not
worth it). PerfBot was not engaged: the parity gate failed first (text sample-independent), so the
perf question is moot.
