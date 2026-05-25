# 0039 — Geode anti-aliasing & coverage: developer reference

**Status:** Developer reference. Geode's edge coverage is **4× MSAA via
`@builtin(sample_mask)`** on the Slug winding-number fill path. The geode↔tiny-skia
sub-pixel coverage floor is **accepted by-design** — geode renders correct content;
the residual diff is the rasterizer-level coverage delta vs tiny-skia's Skia-AAA
scan-converter plus the resvg harness crosshair, proven sample-independent. Several
analytical/supersampling alternatives were evaluated and **rejected** (appendix).

**Related:** [0017 §Phase 4b](0017-geode_renderer.md#phase-4b-in-process-backend-matrix--geode-vs-tiny-skia-parity-comparison),
[0038 text parity](0038-geode_tinyskia_text_parity.md),
[0040 Slug implementation](0040-geode_slug_conformance.md),
[0021 §G5](0021-resvg_feature_gaps.md#g5-audit-the-aa-justified-geode-thresholds)

---

## 1. How Geode coverage actually works

Geode rasterizes vector coverage with the Slug winding-number test evaluated at **4
fixed sub-pixel samples per pixel**, packed into `@builtin(sample_mask)` on a 4× MSAA
target. The hardware resolve averages the surviving samples to produce edge alpha.

All coverage lives in `donner/svg/renderer/geode/shaders/`. The four Slug shaders
share the identical 4-sample machinery:

| Shader | Shapes it covers |
|---|---|
| `slug_fill.wgsl` | solid `drawPath`/`drawRect`/`drawEllipse`, **text glyphs**, decorations, strokes (`strokeToFill`), patterns |
| `slug_gradient.wgsl` | gradient fills/strokes incl. **gradient-on-text** |
| `slug_mask.wgsl` | clip-path coverage masks, `<mask>` luminance |
| `*_alpha_coverage.wgsl` | Intel-Arc + Vulkan fallback mirror of the above |

### 1.1 The 4-sample winding test (`slug_fill.wgsl`)

`fs_main` reads `dpdx`/`dpdy` of the path-space position (constant across the affine
primitive), then for each of 4 fixed sub-pixel offsets (a D3D rotated grid) evaluates
the integer winding test and sets that sample's bit in `sample_mask`:

```wgsl
for (var s: u32 = 0u; s < 4u; s = s + 1u) {
  let sp = in.sample_pos + offsets[s].x*dx + offsets[s].y*dy;
  if (sample_is_inside(band, sp)) { mask = mask | (1u << s); }
}
```

`sample_is_inside` accumulates `curve_winding` over the band's curves and applies the
fill rule: **non-zero** = `winding != 0`, **even-odd** = `(winding & 1) != 0`. The
resolved alpha is `popcount(mask)/4 ∈ {0, ¼, ½, ¾, 1}`. This is the active path on CI's
Mesa llvmpipe and on most adapters (Metal, etc.).

### 1.2 Why `sample_mask` (and the alpha-coverage fallback)

Writing coverage as per-sample `sample_mask` bits is correct **by construction**: each
MSAA sample is written exactly once and the resolve is hardware-additive per sample, so
coverage composes correctly even where multiple fragments (e.g. adjacent encoder bands)
touch the same pixel.

The Intel-Arc + Vulkan path (`*_alpha_coverage.wgsl`, gated in `GeodeDevice.cc` on
`vendorID==0x8086 && backendType==Vulkan`, `useAlphaCoverageAA_`) avoids the
`sample_mask` write (which hangs Mesa ANV). It runs the identical 4-sample loop and
folds `coverage = countOneBits(mask)/4.0` into the fragment color — byte-identical
5-level output to the MSAA path, just moved out of the hardware resolve. (0017 wants
these variants deleted once Mesa 25.3 lands and the hang is gone.)

### 1.3 The per-pixel scale and Slug root-finding

The same Bézier root-finding that the coverage test uses is the Slug pipeline core —
banding, ray/root-find, `solve_quadratic` (numerically-stable Citardauq form), curve
classification, fill rule, fixed-point encoding. That is documented as a unit in
[0040 — Geode Slug implementation](0040-geode_slug_conformance.md); this doc covers
only the **coverage / AA** aspect.

---

## 2. The accepted sub-pixel coverage floor

Geode renders the **correct** shapes/glyphs/colors on every "edge-floor" parity test;
the geode↔tiny diff sits at **101–763 px** — just over the flat-100 parity bar. The
difference is a thin band hugging shape edges, and it is **accepted by-design**:

- It is **not 4× MSAA quantization.** Proven sample-independent: 16 and 64 effective
  samples give **identical** text px (~290), and 16× crushes the 5-level quantization
  entirely (filled shape regions go to 0–8 px). The residual is *algorithm-level* — the
  delta between geode's coverage and tiny-skia's Skia-AAA scan-converter — not a sample
  count.
- It is **not "just AA"** in the hand-wave sense. pixelmatch runs with
  `includeAA=false`; on the diff PNGs the glyph edges are **yellow** (correctly
  excluded as AA), and the only **red (counted)** pixels are the resvg template's
  **0.5px crosshair + 1px frame** overlay, whose sub-pixel placement comes from
  tiny-skia's `snapY` quarter-pixel quantization (`TinyCoverage`) — not reproducible
  per-fragment on the GPU.
- It is **sub-perceptual content.** Per edge pixel the raw alpha delta is ~7/255
  (~2.7%), symmetric (mean signed bias ≈ −1, i.e. no positional shift). The gates trip
  only because that delta accumulates over long glyph/shape perimeters plus the
  crosshair overlay.

**What this means for a developer:** an edge-floor gate is a *non-bug*. The content
matches; do not try to "fix" it with finer AA, and do not relax the parity threshold to
absorb it. If a parity diff lands as a **solid region** (not an edge band / crosshair),
that is a real bug — fix it, don't classify it as edge-floor.

### 2.1 The floor vs tiny-skia's Skia-AAA — what differs

tiny-skia uses a scan-converter (Skia-AAA-style, ~16-sample finite coverage with
`snapY` quarter-pixel quantization). Geode uses 4× MSAA `sample_mask`. The two produce
the same content but distribute partial-edge coverage differently:

- **Vertical/near-vertical edges:** both resolve well; the horizontal coverage
  distribution differs sub-perceptually.
- **Near-horizontal edges + the 0.5px horizontal crosshair:** tiny distributes the
  thin line asymmetrically across rows (e.g. 128/192); geode's symmetric resolve gives
  160/160 — a geometric line-placement difference, not a coverage-quality one.

This is a deliberate rasterizer difference, recorded so it isn't repeatedly
re-diagnosed as a bug.

---

## 3. How the floor is gated (not masked)

The edge-floor tests stay **binary-gated** in the `GeodeTinyParity` parity mode
(`resvg_test_suite.cc`), with no per-test thresholds and no sample-count bump — per the
firm no-masking policy ([0017 §Phase 4b](0017-geode_renderer.md#phase-4b-in-process-backend-matrix--geode-vs-tiny-skia-parity-comparison)).
Their reason points at this doc: *"geode-vs-tiny AAA coverage / crosshair sub-pixel
delta; content matches, unfixable in-renderer."*

The parity metric stays at **0.02** (pixelmatch perceptual YIQ, `includeAA=false`); the
threshold sweep (appendix) showed no clean threshold absorbs the floor without masking
genuine diffs, so it was not relaxed.

> If reference-grade edge AA is ever required (e.g. a print/export path), the only
> viable approach is a GPU compute-shader port of Skia-AAA writing a coverage texture
> (appendix option C) — a new rasterizer, scoped separately. It is not worth it for the
> resvg parity corpus.

---

## Appendix — rejected approaches (do not re-attempt)

Four coverage rewrites and a threshold sweep were built, measured, and rejected during
the parity push. Each is recorded with its killer reason so the work isn't repeated.

### Rejected: dual-ray analytical coverage (Slug Eq. 3, two axis-aligned rays)

Slug's published AA computes fractional coverage from each Bézier/ray crossing's signed
distance to the pixel center (Eq. 3, `f = sat(m·Cx + ½)`), one horizontal + one
vertical ray averaged for isotropy. A single-horizontal-ray POC roughly halved the text
edge floor (`simple-case` 708→~290) and was correct for non-text geometry (0–18 px), but
**cannot reach ≤100 px** vs tiny: the resvg harness's 0.5px axis-aligned crosshair is the
killer — a ray cast *along* a sub-pixel line never exits it and reports full coverage
(255 vs tiny's 160), while a perpendicular glyph fringe needs the opposite combine. No
single combine (average / min / min-max hybrid) satisfies both; the best hybrid was net
**+10 / −16** gates. A second perpendicular ray also requires **vertical bands** from the
encoder, and faking it by swizzling the horizontal band's curves blew up to ~45 000 px
(wrong curve list). Implemented and reverted twice.

### Rejected: single-axis analytical supersampling (Slug paper §6 "quality mode")

One ray + N sub-samples shifted perpendicular to it, averaged — meant to sidestep the
thin-line conflation. NO-GO for two reasons. **(A)** It proved tiny-skia is
*finite-sample*, not analytically smooth: `simple-case` gave N=4 → 15 px but N=8/N=16 →
411 px (a converging average cannot get *worse* with more samples). So matching tiny
means matching its specific ~16-sample pattern, not computing the analytical ∞-sample
limit (which differs by >0.02 by construction: 1/16 = 0.0625). **(B)** Folded-alpha
coverage doesn't compose across encoder band seams: a pixel straddling a band boundary
is shaded once per band, each writing ~0.5 into alpha, which premultiplied source-over
composes as 0.75 instead of 1.0 — ~168–182 tests regressed identically at every N. This
band-seam additivity loss is the same root that killed dual-ray.

### Rejected: 16× MSAA `sample_mask`

Raising the `sample_mask` path to 16 samples would keep hardware-additive composition
(no seam bug) and target tiny's ~16-sample count. NO-GO: WebGPU only guarantees
`sampleCount ∈ {1, 4}`; wgpu-native on Metal (M4 Pro) and llvmpipe both cap at 4, and
16× MSAA resolve already showed driver hangs on Intel Arc (the reason the alpha-coverage
fallback exists). Unreachable as true MSAA.

### Rejected: render-supersample (N×-larger target + box-downsample)

The only adapter-portable way to reach 16/64 effective samples. NO-GO: text parity is
**sample-independent** (16 eff and 64 eff both ~290 px — Blocker A above held for text
even though filled shapes reached 0–8 px), so it fails the parity gate for the right
reason. It is also a pervasive rewrite — offscreen targets (filter/layer/mask/pattern/
image) and every blit/scissor/clip-rect coordinate would need the N× factor; a kSS=4
sweep gave 0 flips / 200 regressions (offscreen content composites at 1× into the kSS×
target). Perf: kSS=2 ≈ 4× fill cost, kSS=4 ≈ 16× — moot given the parity failure.

### Rejected: threshold relaxation

Sweeping the parity threshold to absorb the floor: 0.022–0.030 flips only 1/191; the
cliff is at **0.10** (flips 165) but that is masking-adjacent (absorbs real ~10%
solid-region diffs) and 0.30 masks the genuine feGaussianBlur bug. There is no clean
minimal threshold; relaxing was not authorized (firm no-masking policy). Kept at 0.02.

### If reference-grade AA is ever needed (recorded, not pursued)

- **Option C — GPU compute-shader Skia-AAA port:** write a coverage texture from a
  from-scratch scan-converter match. The only approach that could bit-match tiny, but a
  new rasterizer; scoped separately, not worth it for the resvg corpus.
- **Non-overlapping band ownership** (full-height band quads / single-band encoding for
  small paths) would let folded-alpha analytical coverage compose correctly and re-enable
  the 1-sample perf dream — but still wouldn't match tiny's finite quantization (Blocker
  A), so it trades parity for perf, not for parity.

### Why the original "analytical AA is a perf win" framing didn't pan out

The investigation initially argued analytical 1-sample coverage would be both smoother
*and* cheaper than 4× MSAA (no MSAA color attachment / resolve, fewer root solves per
pixel). The perf argument is sound in isolation, but it was **moot** — every folded-alpha
scheme that would have realized it loses cross-band composition additivity (Blocker B),
and tiny being finite-sample (Blocker A) means analytical coverage can't reach parity
regardless. The `sample_mask` MSAA path stays because it is correct by construction.
