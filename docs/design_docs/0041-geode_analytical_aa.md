# 0041 — Geode anti-aliasing & coverage: analytic Slug alignment

**Status:** **Implemented (as-built).** Geode now computes **official Slug analytic
dual-ray coverage at 1 sample/pixel on every adapter** — the 4× MSAA `sample_mask` path
and the Intel-Arc alpha-coverage fallback are deleted, the Mac/Linux path split is gone,
and `GeodeTinyParity` is retired (tiny-skia is a finite-sample scan-converter; the
correct analytic result is not forced to bit-match it — each backend gates against the
shared reference via `GeodeGolden`/`TinyGolden`). Landed across `c7dae609` (dual-ray
fill/gradient/mask + sampleCount=1 + parity retirement + golden regen), `7a95b98f`
(perf ceilings), `4346dd43` (alpha-coverage deletion), and `6a925e93` (the follow-on
bug fixes that the analytic rewrite *revealed* were never coverage issues — see note).

> **Important correction (post-implementation):** the 16 resvg tests that were gated as
> "slug_fill edge-coverage" were **misdiagnosed** — the analytic rewrite left them
> byte-identical, proving coverage was never the cause. They were three real bugs (a
> pattern-tile filter-region-scissor leak, a missing feMorphology linearRGB round-trip,
> and a degenerate zero-area closed-stroke decomposition) plus two cases where Geode is
> verified-correct but differs from resvg's finite-sample reference (per-backend Geode
> goldens, the legitimate "Donner renders higher quality" pattern). All 16 are
> un-skipped and green. `feGaussianBlur/complex-transform` (genuine
> analytic-vs-finite-sample 1px blur edge, §6) uses a verified per-backend Geode
> golden, not a coverage suppression: its per-pixel analytic coverage on the rotated
> edge is a correctly-centered 1px box-filter ramp and the filled area matches a 16×
> scan-conversion to <0.3px. The resvg finite-sample reference differs from the
> analytic ideal, and the directional `stdDeviation="12 0"` blur amplifies that
> sub-pixel edge into a thin ~1px band (~259px; 700× 1px-runs, max 4px). There are
> **zero** `disableBackend(Geode)` resvg gates for coverage reasons.

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

**Verbatim per-ray accumulation** (reference `SlugRender`, horizontal ray — vertical is
the transpose using `SolveVertPoly`, `pixelsPerEm.y`, `ycov`/`ywgt`):
```hlsl
float2 pixelsPerEm = 1.0 / fwidth(renderCoord);   // path-units → pixels, per axis
float xcov = 0.0, xwgt = 0.0;
for (curveIndex in hband) {
  float4 p12 = curve.p0p1 - renderCoord;          // control pts relative to the pixel
  float2 p3  = curve.p2   - renderCoord;
  uint code = CalcRootCode(p12.y, p12.w, p3.y);    // 0x2E74 winding classifier (y-signs)
  if (code != 0U) {
    float2 r = SolveHorizPoly(p12, p3) * pixelsPerEm.x;   // root x-distances in pixels
    if ((code & 1U) != 0U) { xcov += saturate(r.x + 0.5); xwgt = max(xwgt, saturate(1.0 - abs(r.x)*2.0)); }
    if (code > 1U)         { xcov -= saturate(r.y + 0.5); xwgt = max(xwgt, saturate(1.0 - abs(r.y)*2.0)); }
  }
}
```
Key pieces to port faithfully in M4:
- **`CalcRootCode(y0,y1,y2)`** — the `0x2E74` lookup indexed by the sign bits of the three
  control-point Y's (relative to the pixel) returns which of the two roots are eligible
  crossings and their winding sign. This *replaces* Geode's current `dy/dt`-sign winding
  in `curve_winding`. The exact bit math (verbatim):
  ```
  uint shift = (i2 & 2U) | (i1 & ~2U);
  shift = (i3 & 4U) | (shift & ~4U);
  return ((0x2E74U >> shift) & 0x0101U);   // bit 0 → root x eligible, bit 8 → root y eligible
  ```
- **Per-root coverage** `saturate(r + 0.5)`, signed `+`/`−` by which root (encodes winding
  direction); **per-root weight** `saturate(1 − 2·|r|)` (1 at a crossing through the pixel
  center, 0 once the crossing is ≥ ½px away — this is what makes the combine pick the ray
  whose crossing is *nearest* the pixel).
- **`pixelsPerEm = 1/fwidth(renderCoord)`** — Geode's path-space `sample_pos` is the
  `renderCoord`; `fwidth(sample_pos)` already gives path-units-per-pixel per axis, so this
  is correct under arbitrary affine transforms (replaces the current `dpdx`/`dpdy` length).
- **Fill rule:** non-zero is `saturate(coverage)`; even-odd uses the reference's
  triangle-wave fold of `coverage` — port it from the reference's even-odd path.

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
**Decision: (a).** `GeodeGolden` (geode vs the resvg reference PNG) is the strict
**correctness** gate and stays at the strict budget — analytic Slug must pass it; it is
the real "did we render the right thing" test, and the 0021/0017 no-masking policy holds
for it unchanged. The **`GeodeTinyParity`** mode (geode pixel-vs-tiny) is **dropped for
the resvg corpus**: it compared two legitimately-different rasterizers (Slug analytic vs
Skia-AAA finite-sample) and, per §6 Blocker A, can never be satisfied without degrading
Geode to tiny's quantization. Both backends already gate against the *same* reference via
`GeodeGolden` / `TinyGolden`, so geode-vs-tiny adds no correctness signal the goldens
don't — only a false-failure floor.
- **Why not (b) edge-tolerant parity:** an edge-band-tolerant geode-vs-tiny comparison is
  extra harness complexity that still encodes "geode should look like tiny," which is the
  wrong target now that Slug is the reference we align to. Rejected.
- **Scope of the drop:** only the `GeodeTinyParity` *instance* in `resvg_test_suite.cc`'s
  parameterization. `GeodeGolden` and the non-resvg Geode regression/golden tests
  (`renderer_regression_tests_geode`, `geode/tests/*`) are untouched and stay strict. A
  one-line note in the suite records that geode-vs-tiny parity was retired in favor of
  per-backend golden comparison (this doc is the reference).
- This **needs owner sign-off** before M6 lands (it changes what "parity" means for the
  corpus) — flagged here as the design's choice; the owner has directed aligning to Slug,
  which (a) operationalizes.
- Self-verifiable: after M1 the current edge-floor gates come off and the suite stays
  green **with today's 4× MSAA shader**, because `GeodeGolden` already passes content.
  Any of the 16 that *also* fail `GeodeGolden` (e.g. feImage/svg was 1787px) are genuine
  and stay gated until M4 — that residual list is M4's acceptance set.

### M2 — Non-overlapping band ownership (clears Blocker B, §5)
**Resolved: fold into the single-quad design (§8.1).** Instead of per-band quads (which
overlap at seams under dilation and double-count folded coverage), the path draws **one
bounding quad** and the fragment looks up its H-band (by Y) and V-band (by X) from dense
band grids. Each output pixel is then rasterized by **exactly one** fragment, so folded
sampleCount=1 coverage is additive-free by construction — no per-sample band ownership, no
seam reconciliation. This is also exactly what the dual-ray shader (M4) needs (a pixel
needs both its bands), so M2 and M3c/M4 share the single-quad rewrite rather than being
separate steps. (The reference Slug renderer draws one quad per glyph and looks up the
band per-fragment for the same reason.)

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
- **M1** is resolved to (a) above; it still needs owner sign-off before M6 lands.
- **`CalcRootCode` port:** the `0x2E74` classifier must be ported exactly (it encodes
  both root eligibility *and* winding sign). A wrong port is the most likely source of a
  fill-rule or thin-feature bug; unit-test it against the existing integer `curve_winding`
  on a corpus of curves (they must agree on the set of eligible crossings and signs).

## 8. Concrete implementation design

This section makes M3c/M4/M5 unambiguous. It is the as-designed target; M3b (X-monotonic
split + `vBands`/`vCurves` in the encoder) already landed (`4aba5c09`).

### 8.1 Data model — `EncodedPath` (dense band grids + single quad)

The fragment must find its band in O(1) from `sample_pos`, so bands are stored **densely**
(one slot per grid cell; empty cells carry `curveCount = 0`). Replace the packed, empty-
skipped layout with:

```cpp
struct EncodedPath {
  struct Curve { float p0x,p0y, p1x,p1y, p2x,p2y; };   // unchanged
  struct Band  { uint32_t curveStart, curveCount; };   // SHRINKS: no per-band x/yMin/Max
                                                        //   (the grid params give strips)
  // Horizontal grid (horizontal ray): hBands[i] owns the Y-strip
  //   [yBase + i*hStride, yBase + (i+1)*hStride). hBands.size() == hBandCount (dense).
  std::vector<Curve> hCurves;  std::vector<Band> hBands;
  float yBase; float hStride; uint32_t hBandCount;
  // Vertical grid (vertical ray): vBands[j] owns the X-strip [xBase + j*vStride, …).
  std::vector<Curve> vCurves;  std::vector<Band> vBands;
  float xBase; float vStride; uint32_t vBandCount;
  // Single bounding quad (6 verts) over the dilated pathBounds; corner normals only.
  std::vector<Vertex> vertices;   // Vertex { posX,posY, normalX,normalY } — bandIndex REMOVED
  Box2d pathBounds;
};
```

Notes:
- **Dense vs packed:** dense costs one `Band{start,count}` (8 B) per empty cell; bounded by
  the 256-band cap → ≤ 2 KB/axis worst case. Worth it for branchless O(1) fragment lookup.
  (Keep the existing skip-empty packing only if a band-index side table is added instead;
  dense is the simpler correct default.)
- The current per-band-quad `vertices` (6× per band) collapses to **one quad** for the
  whole path → each pixel is rasterized by exactly one fragment → **Blocker B (seam
  additivity) cannot occur** (this *is* M2, folded into the single-quad design).
- `m3b` shipped `vBands`/`vCurves` with transposed-field `Band`s; 8.1 supersedes that
  `Band` layout — drop the x/y extents (grid params replace them) when M3c lands.

### 8.2 Encoder (`GeodePathEncoder`)

- Emit dense `hBands` (size `hBandCount`) and `vBands` (size `vBandCount`); a cell with no
  curves gets `{curveStart=<next>, curveCount=0}`.
- Set `yBase/hStride/hBandCount` and `xBase/vStride/vBandCount` from `pathBounds` and
  `computeBandCount`.
- Emit one dilatable quad: corners `(pathBounds.{min,max})` with outward corner normals
  `(±1,±1)` (the existing dilation vertex math is unchanged).
- Degenerate axis (zero width/height): set that axis's `*BandCount = 0`; the shader skips
  that ray and the combine falls back to the other (a hairline is covered by one ray).

### 8.3 Shaders (`slug_fill` / `slug_gradient` / `slug_mask`)

Single WGSL per primitive (no `_alpha_coverage` variant). Bindings gain the second
curve+band SSBO and the grid params (in the uniform block).

**Vertex:** unchanged dilation; output path-space `sample_pos`. No `bandIndex` varying.

**Fragment:**
```
ppem = 1.0 / fwidth(sample_pos);                       // path→pixel scale, per axis
hi = clamp(i32((sample_pos.y - yBase)/hStride), 0, hBandCount-1);
vj = clamp(i32((sample_pos.x - xBase)/vStride), 0, vBandCount-1);
(xcov,xwgt) = accumulateHoriz(hBands[hi], sample_pos, ppem.x);   // §1 verbatim loop
(ycov,ywgt) = accumulateVert (vBands[vj], sample_pos, ppem.y);
coverage = CalcCoverage(xcov,xwgt,ycov,ywgt);          // §1, then fill-rule fold
coverage *= clipPolygonCoverage(pixel) * clipMaskCoverage(pixel);   // existing, unchanged
out.color = premultipliedPaint * coverage;             // solid/gradient/pattern as today
```
- `accumulateHoriz/Vert` port the §1 verbatim loop incl. `CalcRootCode` (`0x2E74`) and
  `SolveHorizPoly`/`SolveVertPoly` (reuse the existing `solve_quadratic` Citardauq core).
- **Mask (`slug_mask`) union semantics:** today it packs one sub-sample per RGBA channel
  and unions overlapping mask draws with `BlendOperation::Max`. With a single analytic
  coverage `c`, write `vec4(c,c,c,c)` and keep `Max` blend → overlapping coverage unions as
  `max(c1,c2)` per channel (correct union; no double-count). The mask *reader*
  (`clip_mask_coverage`) already averages the 4 channels → returns `c`. So the Max-union
  invariant is preserved with no reader change.

### 8.4 Pipeline / upload / bind groups (`GeoEncoder`, `GeodePipeline`, `GeodeShaders`)

- **`sampleCount = 1`** for all three pipelines; delete the MSAA color target + resolve in
  `GeoEncoder` (the `sampleCount>1` branches) and the `useAlphaCoverageShader`/`sampleCount`
  ctor params in `GeodePipeline`/`GeodeShaders`.
- Bind-group layout adds: `vCurves` SSBO, `vBands` SSBO (the H ones already exist). Grid
  params (`yBase,hStride,hBandCount,xBase,vStride,vBandCount`) go in the existing uniform
  block (pad to 16 B).
- `GeoEncoder::submitFillDraw` / `fillPathIntoMask` upload both band/curve sets into the
  arenas and `draw(6)` (one quad) instead of `draw(vertices.size())`.

### 8.5 Deletion gates (M5b — same PR that makes each unused)
`slug_fill_alpha_coverage.wgsl`, `slug_gradient_alpha_coverage.wgsl`,
`slug_mask_alpha_coverage.wgsl`; `useAlphaCoverageAA_` (`GeodeDevice.cc`) + its adapter
probe; `sampleCount()`/`useAlphaCoverageShader` MSAA plumbing (`GeodeDevice.h`,
`GeodePipeline.*`, `GeodeShaders.*`, `GeoEncoder.cc` MSAA target/resolve).

### 8.6 Test & golden plan
- **Unit:** `CalcRootCode` vs integer `curve_winding` agreement (8.7 risk); the existing
  M3b winding-parity test stays.
- **Acceptance (M4):** the 16 gated entries pass `GeodeGolden` at the strict budget; diff
  PNGs show edge movement toward the reference; the crosshair case (§6) verified.
- **Golden regen:** `UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run --config=geode
  <golden target>` **after** eyeballing a sample of diffs confirms movement toward
  reference (not arbitrary). Record which goldens changed.
- **Full:** `bazel test //...` green on `resvg_test_suite_{geode,default_text,max}`,
  `renderer_regression_tests_geode`, `geode/tests/*`. Un-skip the 16; retire
  `GeodeTinyParity` from the resvg parameterization (M1 (a)).
- **Perf:** `donner_perf_cc_test` before/after on the splash drag + a text-heavy scene.

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
