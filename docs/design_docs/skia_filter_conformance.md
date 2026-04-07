# Design: Skia Filter Conformance

**Status:** In Progress
**Author:** Claude Opus 4.6
**Created:** 2026-04-03
**Last Updated:** 2026-04-04
**Tracking:** Depends on [#151](https://github.com/jwmcglynn/donner/issues/151)

## Summary

Close all gaps in the Skia backend's SVG filter rendering so that
`bazel test //donner/svg/renderer/tests:resvg_test_suite --config=skia` passes
every non-UB test without skips or inflated thresholds.

The Skia backend started with **193 failing filter tests**. After the fixes
described below, **6 remain** (187 fixed, 97%) in the current worktree
validated on 2026-04-04. The TinySkia backend is not regressed.

## Goals

- All resvg filter tests pass on `--config=skia` without skips or threshold
  overrides (UB-labeled tests remain skipped per project convention).
- The existing TinySkia test results are not regressed.
- All filter processing uses native Skia APIs (`SkImageFilter`, `SkSurface`,
  `SkImage`).
- Clean up threshold overrides that are no longer needed.

## Non-Goals

- GPU-accelerated filter pipeline (Vulkan/Metal/WebGPU) — future work.
- CSS `backdrop-filter`.
- `BackgroundImage`/`BackgroundAlpha` standard inputs (deprecated SVG 1.1).
- Filter Effects Module Level 2 features.
- External resource loading for `feImage`.

## Architecture

### Key Insight: SkImageFilter Bounds Problem

Skia's `saveLayer` + `SkImageFilter` sizes the offscreen layer based on the
filter's **input bounds** (`SkImageFilter::getInputBounds`). Generator filters
like `feFlood` and `feTurbulence` report `Empty()` input bounds (they need no
source pixels), so the layer is undersized — it only covers the SourceGraphic's
drawn content area, not the full filter region.

### Implemented Architecture

Instead of using `saveLayer` with the SkImageFilter directly, we:

1. **`pushFilterLayer`**: Capture the SourceGraphic into an offscreen
   `SkSurface` (full viewport size). Redirect only the paint output to this
   surface. Clip-path and mask remain outside the filter layer so they apply
   to the filtered result, not the SourceGraphic input.

2. **`popFilterLayer`**: Snapshot the SourceGraphic as `SkImage`. Build the
   native `SkImageFilter` DAG via `buildNativeSkiaFilterDAG`. Apply the filter
   by calling `drawImage(sourceImage, 0, 0, &filterPaint)` with `resetMatrix()`
   on the parent canvas. Crop to the filter region via `SkImageFilters::Crop`.

```
RendererDriver
  ├── pushFilterLayer(filterGraph, filterRegion)
  │     ├── Create offscreen SkSurface (full viewport)
  │     ├── Set offscreen canvas matrix = parent CTM
  │     ├── Clip SourceGraphic input to filter region
  │     └── Redirect drawing to offscreen canvas
  └── popFilterLayer()
        ├── Restore parent canvas
        ├── Snapshot offscreen → SkImage (SourceGraphic)
        ├── buildNativeSkiaFilterDAG() → SkImageFilter DAG
        │   ├── All spatial params scaled by deviceFromFilter
        │   ├── Per-node CropRect for explicit subregions
        │   └── Per-node linearRGB wrapping (SRGBToLinearGamma / LinearToSRGBGamma)
        ├── Wrap in SkImageFilters::Crop(filterRegion)
        ├── resetMatrix() on parent canvas
        └── drawImage(sourceImage, 0, 0, &filterPaint) with SkImageFilter
```

Since the filter DAG operates on the device-pixel SourceGraphic image (captured
at full resolution), all spatial parameters must be in device-pixel coordinates.
The `deviceFromFilter` transform (= canvas CTM at filter push time) is used to
scale user-space values to device pixels.

### Key Design Decisions

**D1: Offscreen SkSurface instead of saveLayer.**
Skia's saveLayer cannot allocate space for generator filter output beyond the
SourceGraphic bounds. Using an explicit SkSurface + drawImage gives full control
over bounds while still using native SkImageFilter for all processing.

**D2: All filter operations are native Skia.**
Every filter primitive is lowered to Skia's `SkImageFilter` API. The offscreen
surface is only used to capture the SourceGraphic; the actual filter computation
uses Skia's optimized implementations.

**D3: Device-space parameters.**
Since `drawImage` with `resetMatrix()` operates in device-pixel coordinates,
all filter parameters (offsets, radii, frequencies, light positions, subregions)
are pre-transformed from user-space to device-pixel-space.

**D3a: Filter inputs are clipped before filtering; clip-path and mask are not.**
The implicit `SourceGraphic` input is cropped to the filter region before
running the Skia filter DAG. Entity clip-path and mask remain outside the
filter layer so they apply in SVG order: paint, then filter, then clip-path,
then mask.

**D4: SkColor4f is always unpremultiplied.**
Skia's `SkColor4f` API expects straight (unpremultiplied) RGBA values. The
canvas bitmap is `kUnpremul_SkAlphaType`. Colors passed to `SkShaders::Color`
and `SkColorSetARGB` must be in straight sRGB — Skia handles premultiplication
internally.

## Implementation Plan

### Phase 1: Architecture + Coordinate Transform (DONE)

- [x] Replace `saveLayer`+`SkImageFilter` with offscreen `SkSurface` capture +
  `drawImage` with filter paint
- [x] Scale all spatial params by `deviceFromFilter`: blur sigma, offset dx/dy,
  morphology radius, displacement scale, turbulence frequency (inverse),
  lighting positions, tile rects, image viewport
- [x] Transform subregions to device-pixel coordinates for feTile and feImage
- [x] Crop filter region in device coordinates via `SkImageFilters::Crop`
- [x] Per-node CropRect for nodes with explicit x/y/width/height subregions

### Phase 2: Input Order + Color Space (DONE)

- [x] Fix `feBlend`/`feComposite` input order: SVG `in`=foreground,
  `in2`=background → Skia `Blend(background, foreground)`
- [x] Fix `if(in2)` null check skipping `SRGBToLinearGamma` on SourceGraphic
  (`nullptr` is valid in Skia's SkImageFilter = source image)
- [x] feColorMatrix: fix identity matrix for empty `values` attribute
- [x] feComponentTransfer: fix single-value table (constant output, not identity)

### Phase 3: Color Values + Premultiplication (DONE)

- [x] feFlood: pass straight RGBA to `SkColor4f` (was incorrectly premultiplied
  with flood-opacity, causing darkened semi-transparent colors)
- [x] feFlood: skip linearRGB post-wrap (sRGB output, Skia handles internally)
- [x] feDropShadow: pass straight sRGB to `SkColorSetARGB` (no manual linear
  conversion needed)
- [x] feImage: skip linearRGB post-wrap (image data already in sRGB)
- [x] Lighting: convert light color sRGB→linear when `nodeUsesLinearRGB`

### Phase 4: Lighting Conformance (DONE)

- [x] Remove `surfaceScale * pixelScale` — Skia computes normals from device
  pixels, the pixel-level alpha differences already reflect device resolution
- [x] No-light-source case: produce transparent output instead of failing DAG
- [x] OBB light coordinate resolution for x/y/z and pointsAt positions
- [x] Negative spotExponent: clamp to default (1.0) per SVG spec

### Phase 5: Primitive Validation (DONE)

- [x] feMorphology: zero/negative radius produces transparent output
- [x] feMorphology: OBB radius scaling through bbox dimensions
- [x] feConvolveMatrix: negative order, incomplete kernel, out-of-range target
  → transparent output
- [x] feConvolveMatrix: explicit `divisor="0"` → transparent output
- [x] Effective subregion tracking: inherit from input when no explicit
  x/y/width/height (Offset translates, Blur expands, generators use filter
  region)

### Phase 6: feTile + feImage Fixes (DONE)

- [x] **feTile**: Fixed subregion handling so SkImageFilters::Tile sees
  generator filter content correctly (commit c3890eca)
- [x] **feImage**: Fixed subregion mapping edge cases (009, 021)
- [x] **feFlood-006**: Fixed subregion+offset case
- [x] **Misc primitives**: Fixed fePointLight, feOffset, feDiffuseLighting
  failures

### Phase 7: Two-Input Subregion Union (DONE)

- [x] **feBlend/feComposite**: Fixed default subregion for two-input primitives
  to use the union of both input subregions (SVG spec §15.7.2). Previously used
  only `previousSubregion`, which cropped SourceGraphic to the feFlood's small
  subregion. Fixed: feBlend-007/008, feComposite-001/007/010/011.
- [x] **feMerge subregion**: Merge now unions all input subregions.
- [x] **feDisplacementMap**: Also uses input union for default subregion.

### Phase 8: Buffer Offset Compensation (DONE)

- [x] **feMerge buffer offset**: `pushFilterLayer` set the buffer offset translate
  on the filter canvas, but `setTransform` (called by the rendering driver)
  replaced the entire matrix without it. Fixed by applying `postTranslate` in
  `setTransform` when inside a filter layer with a non-zero buffer offset.
  Fixed: feMerge-001/002/003.

### Phase 9: Source Clipping + OBB Blur + Effect Order (DONE)

- [x] **SourceGraphic clipping**: Crop the implicit `SourceGraphic` input to
  the filter region before Skia primitives run. Fixed:
  `e-filter-002/003/004/017/018`.
- [x] **OBB blur scaling**: Apply `primitiveUnits="objectBoundingBox"` scaling
  to blur sigmas and subregion expansion in the native Skia path. Fixed:
  `e-filter-012/030/055`.
- [x] **Effect stack ordering**: Stop replaying clip-path into the filter
  capture and reorder masked filtered content so the effective SVG order is
  paint → filter → clip-path → mask. Fixed: `e-filter-052/054`.

### Phase 10: Transformed Blur Under Skew (DONE)

- [x] **Skewed blur chain path**: For eligible skewed blur chains, resample the
  captured SourceGraphic into an axis-aligned local Skia surface, run the
  native Skia filter DAG there, and composite back through the original
  transform. Fixed: `e-filter-026`.

### Phase 11: Remaining Work

- [x] **feTurbulence (10)**: Replaced Skia's native noise with SVG-spec Perlin
  noise generator (ported from tiny-skia). Key insight: do NOT set
  `skipLinearRGBPostWrap` — the noise must go through the normal linearRGB
  color space handling, matching how Skia's native shader works.
- [x] **feSpotLight (5)**: Custom SVG-spec diffuse spotlight lighting replaces
  Skia's native SpotLitDiffuse. Fixes cone anti-aliasing differences and
  user-space cone check under non-conformal transforms (test 012).
  Specular spotlight still uses Skia's native API (custom specular needs
  further investigation for correct premultiplication handling).
- [ ] **feSpecularLighting (3)**: Mixed cluster. Two tests are small
  highlight-shape deviations, while `specularExponent="0"` still diverges
  badly from the expected output.
- [ ] **Remaining per-primitive (2)**: feConvolveMatrix (1), feFlood (1).

Current 2026-04-04 worktree note:
- `feMerge-001/002/003` no longer reproduce in the latest Skia run.
- `feDropShadow-006` also no longer reproduces.
- Keep both areas on the regression watchlist until the current `RendererSkia`
  changes are finalized.

## Progress

| Failures | Delta | Key Fixes |
|----------|-------|-----------|
| 193 | — | Baseline (WIP native lowering, broken fallback) |
| 155 | -38 | SkSurface capture, coordinate scaling, Blend input swap |
| 145 | -10 | feFlood linearRGB, `if(hasIn2)` null check |
| 130 | -15 | feColorMatrix identity, DropShadow/Lighting color |
| 101 | -29 | surfaceScale fix, no-light-source, OBB light coords |
| 92 | -9 | ConvolveMatrix validation, divisor=0, transparent error output |
| 74 | -18 | Morphology validation+OBB, per-node CropRect |
| 73 | -1 | feSpotLight negative specularExponent clamp |
| 59 | -14 | feImage skipLinearRGBPostWrap for sRGB data |
| 54 | -5 | SkColor4f unpremultiplied fix, ComponentTransfer single-value |
| 44 | -10 | feTile subregion fix, feImage edge cases, feFlood-006, misc fixes |
| 38 | -6 | Two-input subregion union for feBlend/feComposite |
| 35 | -3 | Buffer offset compensation in setTransform for feMerge |
| 33 | -2 | Current worktree no longer reproduces feMerge or feDropShadow failures |
| 23 | -10 | Source clipping, OBB blur scaling, and correct filter/clip/mask ordering |
| 21 | -2 | Skia-only transformed local-raster blur path clears the last `e-filter` case |
| 20 | -1 | feGaussianBlur extreme sigma fix |
| 10 | -10 | SVG-spec Perlin noise replaces Skia native noise for feTurbulence |
| 5 | -5 | Custom SVG-spec diffuse spotlight lighting replaces Skia native |

## Remaining Failures (6 total)
- `feGaussianBlur`: 1
  Extreme sigma still diverges.
- `feSpecularLighting`: 3
  Highlight-shape and exponent behavior still differ from expected output.
- `feConvolveMatrix`: 1
  Kernel normalization / zero-sum behavior for `e-feConvolveMatrix-014`.
- `feFlood`: 1
  `primitiveUnits="objectBoundingBox"` subregion sizing for
  `e-feFlood-008`.

### First-Level Triage

- `Spot light parameter behavior` (5)
  Tests: `e-feSpotLight-006/007/008/010/012`
  First hypothesis: Cone-angle and spotlight parameter handling mostly differ
  by small lighting precision, except `012`, which still looks like a larger
  coordinate-mapping issue.
- `Specular lighting exponent behavior` (3)
  Tests: `e-feSpecularLighting-002/006/007`
  First hypothesis: Small highlight-shape differences remain, and
  `specularExponent="0"` still produces a substantially wrong result.
- `Single-primitive stragglers` (2)
  Tests: `e-feConvolveMatrix-014`, `e-feFlood-008`
  First hypothesis: These look isolated: kernel normalization / zero-sum
  handling for convolve, and OBB subregion sizing for flood.

### Failure Details

- **Largest remaining diffs:** `e-feSpecularLighting-007` (168068),
  `e-feFlood-008` (17494).
- **Small-diff precision cluster:** `e-feSpecularLighting-002` (113),
  `e-feSpotLight-006` (157), `e-feSpecularLighting-006` (806),
  `e-feSpotLight-007/008/010` (1373 each).


## Testing and Validation

- **Primary gate:** `bazel test //donner/svg/renderer/tests:resvg_test_suite --config=skia`
- **Regression gate:** `bazel test //donner/svg/renderer/tests:resvg_test_suite`
  (TinySkia) — verified passing after each change
- **Cross-config parity:** Both backends should need the same (or fewer)
  threshold overrides
- **Current snapshot:** `LLM=1 bazel test //donner/svg/renderer/tests:resvg_test_suite
  --config=skia --test_output=errors` on 2026-04-04 produced 6 remaining
  failures in the current worktree
