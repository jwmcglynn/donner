# 0041 — Geode anti-aliasing & coverage: analytic Slug alignment

**Status:** Active implementation plan + developer reference. Geode is moving from its
current **4× MSAA `sample_mask`** edge coverage to the **official Slug analytic
dual-ray coverage** computed at **1 sample/pixel on every adapter** — aligning with the
Slug algorithm instead of diverging from it, and removing the Mac (Metal MSAA) vs Linux
(Intel-Arc alpha-coverage) path split. This requires an **encoder change to emit
vertical (X-monotonic) bands** so the vertical ray has data, plus a **rework of the
geode↔tiny parity gate** (tiny-skia is a finite-sample scan-converter; the correct
analytic result cannot and should not be forced to bit-match it).

> **History:** earlier this doc concluded analytic AA was *rejected* and the 4× MSAA
> edge-floor was *accepted by-design*. That conclusion is **reversed**. The technical
> findings behind it are correct and preserved below (§5–§6) — they are the blockers
> this plan must clear, not reasons to stop. The decision now is: **Slug is the
> reference we align to; tiny-skia's finite-sample output is the thing that differs**,
> so the parity *comparison* changes, not the (more-correct) coverage math.

**Related:** [0042 Slug pipeline](0042-geode_slug_conformance.md) (encoder/band
internals — extended by this plan), [0038 text parity](0038-geode_tinyskia_text_parity.md),
[0017 §Phase 4b](0017-geode_renderer.md#phase-4b-in-process-backend-matrix--geode-vs-tiny-skia-parity-comparison),
[0021 §Geode policy](0021-resvg_feature_gaps.md#geode--resvg-override-policy).

---

## 1. The official Slug coverage method (what we align to)

Lengyel's Slug (JCGT 6(2), 2017; **official public-domain reference**
`EricLengyel/Slug` `SlugPixelShader.hlsl`, patent dedicated to public domain
2026-03-17) computes per-pixel fractional coverage at **1 sample/pixel**, analytically.
Verified against the reference shader (`SlugRender` / `CalcCoverage`):

1. **Horizontal ray** through the pixel's **horizontal band** (Y-monotonic curves):
   accumulate an X-coverage `xcov` and a weight `xwgt`. Each root contributes
   `saturate(r + 0.5)` where `r` is the signed root distance from the pixel center
   scaled by `pixelsPerEm` (the screen-space derivative factor, Geode's `dpdx`/`dpdy`).
2. **Vertical ray** through the pixel's **vertical band** (X-monotonic curves): the same,
   producing `ycov`, `ywgt`. The reference fetches a **separate band list** for this,
   offset past the horizontal bands (`bandMax.y + 1`) — i.e. official Slug **does
   maintain X-monotonic vertical bands**; this is not optional, and confirms the
   encoder change (M3) is the correct, on-algorithm path.
3. **Combine** (reference `CalcCoverage`, verbatim):
   ```
   coverage = max( abs(xcov*xwgt + ycov*ywgt) / max(xwgt + ywgt, 1.0/65536.0),
                   min(abs(xcov), abs(ycov)) );
   ```
   The weighted blend handles the general case; the `min(|xcov|,|ycov|)` floor resolves
   the near-axis-aligned / thin-feature / crosshair cases a single ray conflates (§6).

It is exact, resolution-independent, **1-sample, no MSAA, no supersampling** (modern Slug
3.5+ *removed* adaptive supersampling — "A Decade of Slug" — relying on the analytic
coverage plus **dynamic glyph dilation** in the vertex shader, em-space `0.5/fontSize`,
to keep boundary pixels touched at small sizes). Geode already performs the half-pixel
dilation; it implements only step 1's *data* and approximates step 1's coverage with 4
point samples — steps 2–3 (the vertical band + the analytic combine) are missing. Closing
that gap **is** this plan, and the reference gives the exact formula for M4.

> **Does the reference change the plan?** No — it confirms and de-risks it: dual-ray with
> a *separate vertical (X-monotonic) band list* is the real algorithm (so M3's encoder
> change is required, not avoidable); 1-sample/no-MSAA is the real design (so M5's MSAA
> deletion is on-algorithm); and `CalcCoverage` is the exact M4 formula. The optional
> `SLUG_WEIGHT` `sqrt(coverage)` optical-weight boost is gamma, not geometry — skip it.

## 2. Current state (being replaced)

Geode today rasterizes coverage with the Slug winding test at **4 fixed sub-pixel
samples per pixel**, packed into `@builtin(sample_mask)` on a 4× MSAA target; the
hardware resolve averages surviving samples to edge alpha ∈ {0,¼,½,¾,1}.

| Shader | Shapes it covers |
|---|---|
| `slug_fill.wgsl` | solid `drawPath`/`drawRect`/`drawEllipse`, **text glyphs**, decorations, strokes (`strokeToFill`), patterns |
| `slug_gradient.wgsl` | gradient fills/strokes incl. gradient-on-text |
| `slug_mask.wgsl` | clip-path coverage masks, `<mask>` luminance |
| `*_alpha_coverage.wgsl` | Intel-Arc + Vulkan fallback mirror (sampleCount=1, `countOneBits(mask)/4.0`) — same 5-level output, different mechanism |

`fs_main` reads `dpdx`/`dpdy` of path-space position (constant across an affine
primitive), evaluates the integer winding test at 4 offsets (D3D rotated grid), and sets
`sample_mask` bits; `sample_is_inside` applies fill rule (non-zero / even-odd). The
Intel-Arc + Vulkan path (`useAlphaCoverageAA_`, gated on `vendorID==0x8086 &&
Vulkan` in `GeodeDevice.cc`) folds `popcount/4` into color to dodge a Mesa ANV
`sample_mask` hang. Both paths produce identical 5-level coverage. The shared Slug core
(banding, ray/root-find, `solve_quadratic` Citardauq form, classification, fixed-point
encoding) is documented in [0042](0042-geode_slug_conformance.md).

**This entire dual-path, MSAA-based scheme is replaced** by one sampleCount=1 analytic
path (§4). Both the MSAA plumbing and the alpha-coverage variants are deleted (§4 M5).

## 3. Why this aligns the divergence two ways

- **Slug divergence (the important one):** we stop approximating Slug's analytic
  coverage with a 4-sample winding hack and implement the real dual-ray method. Geode
  becomes a faithful Slug renderer.
- **Mac/Linux divergence:** the MSAA vs alpha-coverage split exists *only* to manage 4×
  MSAA across drivers. An analytic sampleCount=1 path needs no MSAA at all, so both
  collapse to one shader on every adapter and the `useAlphaCoverageAA_` Mesa-hang
  workaround is deleted.

## 4. Milestones (mandatory order; each lands on `resvg-test-suite` independently)

Ordering is load-bearing: the seam fix (M2) must precede the analytic shader (M4), and
the gate rework (M1) precedes everything, because otherwise correct analytic output
still "fails" against tiny. The prior autonomous attempt skipped M1+M2 and regressed
every multi-band path (`StructureTransform` rotate/skew, shapes, nested-svg) — that was
Blocker B (§5), not bad luck.

### M1 — Parity-gate rework (enabler, no shader change)
- Keep **`GeodeGolden`** (geode vs the resvg reference PNG) as the strict **correctness**
  gate — analytic Slug must pass it; it is the real "did we render the right thing" test.
- Change **`GeodeTinyParity`** (geode vs tiny-skia) so two valid-but-different
  rasterizers are not failed on sub-perceptual edge deltas: either **(a)** drop
  geode-vs-tiny for the resvg corpus and rely on `GeodeGolden` + tiny's `TinyGolden`
  (both already compare to the same reference), or **(b)** keep it as an
  edge-band-tolerant comparison. **Decision (a) vs (b) needs explicit sign-off** — it
  decides whether "parity" remains a concept for the corpus.
- No relaxation of the **correctness** gate (0021/0017 no-masking policy holds for
  `GeodeGolden`).
- Self-verifiable: after M1 the current edge-floor gates come off and the suite stays
  green **with today's 4× MSAA shader**, because `GeodeGolden` already passes content.
  Any of the 16 that *also* fail `GeodeGolden` (e.g. feImage/svg was 1787px) are genuine
  and stay gated until M4 — that residual list is M4's acceptance set.

### M2 — Non-overlapping band ownership (clears Blocker B, §5)
Make each output pixel's coverage written by **exactly one** fragment so folded
(sampleCount=1) coverage is additive. Candidate mechanisms: full-height band quads with
a per-fragment owning-band test; single-band encoding for small paths; or scissor/
stencil clipping each band quad to its `[yMin,yMax)` with no dilation overlap. Acceptance:
route the **existing 4-sample alpha-coverage shader** to **all** adapters and reproduce
the MSAA path output with **zero** new regressions — isolating the seam fix from the
coverage-math change.

### M3 — Vertical (X-monotonic) bands in the encoder  ← the encoder change
Extend `GeodePathEncoder` to also emit **X-monotonic** quadratics binned into **vertical
bands** (mirror of the existing ~32px Y-banding: split the X range, duplicate each
X-monotonic curve into the vertical bands its X-extent overlaps, cap band count). The
GPU side gets a second band/curve SSBO set (or an interleaved layout) and the fragment
shader gains the vertical-ray winding/coverage. Reuse the Y-band machinery
(`computeBandCount`, the dup-into-overlapping-bands loop, the f32 SSBO encoding) on the X
axis. Acceptance: a debug mode rendering vertical-ray winding alone matches horizontal-ray
winding on closed paths (winding is ray-direction independent), proving the X-band data
is correct before coverage depends on it.

### M4 — Dual-ray analytic coverage shader
Replace the 4-sample loop in `slug_fill`/`slug_gradient`/`slug_mask` with Slug's
analytic coverage (horizontal + vertical ray accumulation + combine, §1) folded into
premultiplied output. Acceptance: M1's acceptance set passes `GeodeGolden` at the strict
budget; diff PNGs show edges moved toward the reference; the crosshair case (§6) is
verified specifically; no full-suite regressions.

### M5 — Unify + delete (explicit deletion gates, no dead code)
In the same PR that makes each unused: delete `slug_fill_alpha_coverage.wgsl`,
`slug_gradient_alpha_coverage.wgsl`, `slug_mask_alpha_coverage.wgsl`; the
`useAlphaCoverageAA_` branch (`GeodeDevice.cc`); the `sampleCount>1` MSAA
target/resolve plumbing (`GeoEncoder.cc`) and the `useAlphaCoverageShader`/`sampleCount`
ctor params (`GeodePipeline.cc`, `GeodeShaders.cc`).

### M6 — Golden regen + un-skip + full verification
Regenerate `testdata/golden/geode/` **only after** diffs confirm movement toward the
reference. Un-skip the 16 in `resvg_test_suite.cc`. `bazel test //...` green on all
variants (geode/default_text/max). Update §2 here to "as-built", and 0042 §1.2 + 0021
totals.

## 5. Blocker B — band-seam additivity (the thing that kills naive attempts)

A pixel straddling two ~32px horizontal bands is shaded once per band. With continuous
folded-alpha coverage, band A writes `a` and band B writes `b`; premultiplied
source-over composes them as `1-(1-a)(1-b)` (e.g. 0.75) instead of the correct `a+b`
(1.0). The 4× MSAA path is immune because per-sample `sample_mask` bits are
hardware-additive and per-sample band-Y ownership routes each *discrete* sample to
exactly one band; continuous coverage has no discrete sample to route. **This is why M2
precedes M4.** Measured previously as ~168–182 identical regressions at every sample
count, and reproduced by the last autonomous attempt's transform/shape regressions.

## 6. Blocker A + the crosshair — why the parity *gate* must change, not the math

- **tiny-skia is finite-sample, not analytic.** Non-monotonic evidence (N=4→15px,
  N=8/16→411px — a converging average cannot get *worse*) proves tiny is a ~16-sample
  scan-converter with `snapY` quarter-pixel quantization. The correct analytic
  ∞-sample result differs from it by >0.02 *by construction* (1/16 = 0.0625). Forcing
  geode to bit-match tiny means deliberately *degrading* to tiny's quantization — the
  opposite of aligning with Slug. Hence M1: compare each backend to the **reference**,
  not to each other.
- **The resvg crosshair.** The harness overlays a 0.5px axis-aligned crosshair. A single
  ray cast *along* a sub-pixel line never exits it and reports full coverage (255 vs
  tiny's ~160). Slug's **dual-ray combine** is the published fix — the perpendicular ray
  resolves what the parallel ray conflates. M4 must verify the crosshair specifically.

## 7. Risks / open questions
- **Perf (hot path):** dual-ray ~doubles per-pixel root solves but removes the MSAA
  resolve and the 4-sample loop. Measure with `donner_perf_cc_test`; net is plausibly
  neutral-to-positive but unproven.
- **Vertical-band memory:** X-banding ~doubles encoded curve data; reuse the existing
  256-band cap.
- **M1 (a) vs (b)** is the highest-leverage decision and needs explicit sign-off.

## Appendix — approaches measured and set aside (now recontextualized)

These were rejected under the *old* constraint "must bit-match tiny-skia." This plan
removes that constraint (M1), so they are reframed as inputs, not dead ends:

- **Dual-ray analytic (Slug Eq. 3):** the *correct* target of this plan. Previously
  blocked because (i) it needs vertical bands the encoder lacked → **now M3**, and
  (ii) it can't bit-match tiny → **now resolved by M1**, not a blocker.
- **Single-axis analytical supersampling:** showed tiny is finite-sample (Blocker A) and
  hit band-seam additivity (Blocker B) → seam handled by **M2**; "match tiny exactly"
  abandoned by **M1**.
- **16× MSAA `sample_mask`:** unreachable — WebGPU caps `sampleCount ∈ {1,4}`. Moot:
  the analytic path is sampleCount=1, no MSAA.
- **Render-supersample (N× target + downsample):** pervasive rewrite, sample-dependent,
  rejected on perf and parity; not revisited.
- **Threshold relaxation of the correctness gate:** still rejected — M1 changes *which
  images are compared*, not the strict budget of the `GeodeGolden` correctness gate.
- **Option C — GPU compute-shader Skia-AAA port:** the only way to *bit-match* tiny; moot
  once M1 stops requiring bit-match. Not pursued.
