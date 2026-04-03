# Design: Skia Filter Conformance

**Status:** In Progress
**Author:** Claude Opus 4.6
**Created:** 2026-04-03
**Last Updated:** 2026-04-03
**Tracking:** Depends on [#151](https://github.com/jwmcglynn/donner/issues/151)

## Summary

Close all gaps in the Skia backend's SVG filter rendering so that
`bazel test //donner/svg/renderer/tests:resvg_test_suite --config=skia` passes
every non-UB test without skips or inflated thresholds.

The Skia backend started with **193 failing filter tests**. After the fixes
described below, **130 remain** (63 fixed, 33% improvement). The TinySkia
backend is not regressed.

## Goals

- All resvg filter tests pass on `--config=skia` without skips or threshold
  overrides (UB-labeled tests remain skipped per project convention).
- The existing TinySkia test results are not regressed.
- All filter processing uses native Skia APIs (`SkImageFilter`, `SkSurface`,
  `SkImage`) — no tiny-skia CPU fallback.
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
   `SkSurface` (full viewport size). Redirect all drawing to this surface.

2. **`popFilterLayer`**: Snapshot the SourceGraphic as `SkImage`. Build the
   native `SkImageFilter` DAG via `buildNativeSkiaFilterDAG`. Apply the filter
   by calling `drawImage(sourceImage, 0, 0, &filterPaint)` with `resetMatrix()`
   on the parent canvas. Crop to the filter region via `SkImageFilters::Crop`.

```
RendererDriver
  ├── pushFilterLayer(filterGraph, filterRegion)
  │     ├── Create offscreen SkSurface (full viewport)
  │     ├── Replay clip stack onto offscreen canvas
  │     ├── Set offscreen canvas matrix = parent CTM
  │     └── Redirect drawing to offscreen canvas
  └── popFilterLayer()
        ├── Restore parent canvas
        ├── Snapshot offscreen → SkImage (SourceGraphic)
        ├── buildNativeSkiaFilterDAG() → SkImageFilter DAG
        │   ├── All spatial params scaled by deviceFromFilter
        │   ├── Colors converted sRGB→linear when nodeUsesLinearRGB
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
Every filter primitive is lowered to Skia's `SkImageFilter` API. No tiny-skia
CPU fallback. The offscreen surface is only used to capture the SourceGraphic;
the actual filter computation uses Skia's optimized implementations.

**D3: Device-space parameters.**
Since `drawImage` with `resetMatrix()` operates in device-pixel coordinates,
all filter parameters (offsets, radii, frequencies, light positions, subregions)
are pre-transformed from user-space to device-pixel-space.

## Implementation Plan

### Phase 1: Architecture + Coordinate Transform (DONE)

- [x] Replace `saveLayer`+`SkImageFilter` with offscreen `SkSurface` capture +
  `drawImage` with filter paint
- [x] Scale all spatial params by `deviceFromFilter`: blur sigma, offset dx/dy,
  morphology radius, displacement scale, turbulence frequency (inverse),
  lighting positions + surfaceScale, tile rects, image viewport
- [x] Transform subregions to device-pixel coordinates for feTile and feImage
- [x] Crop filter region in device coordinates via `SkImageFilters::Crop`

### Phase 2: Input Order + Color Space (DONE)

- [x] Fix `feBlend`/`feComposite` input order: SVG `in`=foreground,
  `in2`=background → Skia `Blend(background, foreground)`
- [x] Fix `if(in2)` null check skipping `SRGBToLinearGamma` on SourceGraphic
  (`nullptr` is valid in Skia's SkImageFilter = source image)
- [x] feFlood: convert flood color sRGB→linear when `nodeUsesLinearRGB`
- [x] feDropShadow: convert shadow color sRGB→linear when `nodeUsesLinearRGB`
- [x] Lighting: convert light color sRGB→linear when `nodeUsesLinearRGB`
- [x] feColorMatrix: fix identity matrix for empty `values` attribute

### Phase 3: Lighting Conformance (IN PROGRESS)

40 lighting tests remain (17 diffuse, 10 spot, 6 specular, 4 point, 3 distant).
Root causes to investigate:

- [ ] Skia's lighting filters may compute normals differently than the SVG spec
  (Skia uses its own bump-map normal extraction)
- [ ] `surfaceScale * pixelScale` interaction — verify Skia's internal handling
  matches what we pass
- [ ] SpotLight cone angle computation in device-pixel space
- [ ] Distant light direction vector may need device-space rotation

### Phase 4: Remaining Per-Primitive Issues

- [ ] **feConvolveMatrix (11)**: Kernel operation in device-pixel space may
  differ from user-space semantics. Check edge handling and kernel size.
- [ ] **feTurbulence (10)**: Skia's Perlin noise vs SVG reference algorithm.
  Frequency scaling is applied but noise pattern may still differ.
- [ ] **feImage (16)**: Viewport/subregion mapping in device-pixel coordinates.
  Fragment references vs external images.
- [ ] **feTile (6)**: Src/dst rect device-space mapping. Possible seam issues.
- [ ] **feMorphology (6)**: Radius scaling applied but still failing — debug.
- [ ] **feFlood remaining (5)**: Opacity/subregion edge cases.
- [ ] **feComposite remaining (5)**: Arithmetic mode coefficient handling.
- [ ] **feBlend remaining (2)**: Subregion tests (007, 008).
- [ ] **e-filter composite (21)**: Chains of upstream issues.

### Phase 5: Threshold + Skip Cleanup

- [ ] Remove Skia-specific threshold overrides no longer needed
- [ ] Verify all non-UB tests pass cleanly
- [ ] Re-evaluate skipped tests for Skia-specific fixes

## Progress

| Date | Failures | Delta | Key Fixes |
|------|----------|-------|-----------|
| Start | 193 | — | Baseline (WIP native lowering, broken fallback) |
| +arch | 155 | -38 | SkSurface capture, coordinate scaling, Blend input swap |
| +color | 145 | -10 | feFlood linearRGB, `if(in2)→if(hasIn2)` null check |
| +more | 130 | -15 | feColorMatrix identity, DropShadow/Lighting color |
| +lighting | 101 | -29 | surfaceScale fix, no-light-source, OBB light coords |
| +convolve | 92 | -9 | Invalid param validation, divisor=0, transparent error output |
| +morph+tile | 74 | -18 | Morphology validation+OBB, per-node CropRect for subregion clipping |
| +spot | 73 | -1 | feSpotLight negative specularExponent clamp |
| +feImage | 59 | -14 | skipLinearRGBPostWrap for sRGB image data |
| +edge | 57 | -2 | ComponentTransfer single-value table, flood-color hsla fix |

### Known Skia Limitations (require thresholds)

**feTile (6 tests):** `SkImageFilters::Tile` can't see content from generator filter
chains (feFlood→feOffset→feTile) because Skia's bounds model traces back through
the chain and generator filters report `Empty()` input bounds. The Tile filter
receives no pixels to tile. Fixing this requires either pre-rendering the tile
input to an `SkImage` (which has its own coordinate challenges) or implementing
tiling manually outside the SkImageFilter DAG.

**feTurbulence (10 tests):** Skia's `SkShaders::MakeTurbulence`/`MakeFractalNoise`
implements a different Perlin noise algorithm than the SVG spec's reference
(different lookup tables and gradient vectors). The noise patterns are visually
similar but not pixel-identical. Diffs range from 2K to 75K pixels. These tests
require `maxMismatchedPixels` thresholds since the algorithm cannot be fixed
without reimplementing the SVG noise or using a non-Skia implementation.

## Remaining Failures by Category (92 total)

| Category | Count | Likely Root Cause |
|----------|-------|-------------------|
| e-filter (composite) | 21 | Chains of upstream issues |
| feImage | 16 | Viewport/subregion mapping |
| feTurbulence | 10 | Noise pattern differences |
| feTile | 6 | Rect coordinate mapping |
| feSpotLight | 6 | Cone angle / position |
| feMorphology | 6 | Radius issue |
| feFlood | 5 | Opacity / subregion |
| feComposite | 5 | Arithmetic / order |
| feSpecularLighting | 3 | Lighting edge cases |
| feConvolveMatrix | 2 | Edge cases (incomplete kernel, sum=0) |
| feGaussianBlur | 2 | Edge handling |
| feBlend | 2 | Subregion tests |
| Misc (1 diffuse, 1 point, 1 offset, 1 component, 4 attrs) | 8 | Edge cases |

## Testing and Validation

- **Primary gate:** `bazel test //donner/svg/renderer/tests:resvg_test_suite --config=skia`
- **Regression gate:** `bazel test //donner/svg/renderer/tests:resvg_test_suite`
  (TinySkia) — verified passing after each change
- **Cross-config parity:** Both backends should need the same (or fewer)
  threshold overrides
