# Geode Phase 3b clip-mask work

## What was broken

Two separate Geode clip-path bugs were involved:

1. **Path-mask passes never opened on Intel's alpha-coverage path.** On `sampleCount()==1`, `GeoEncoder::beginMaskPass()` still required an MSAA mask texture, so arbitrary path clips rendered an all-zero mask.
2. **Filter-layer compositing dropped the active clip-path.** `RendererGeode::pushFilterLayer()` cleared the clip stack before capturing `SourceGraphic`, so `popFilterLayer()` composited filtered content back without the clip behaving like the direct/isolated-layer paths.

## What changed

- Fixed `GeoEncoder::beginMaskPass()` so the alpha-coverage path can render directly into the resolve mask texture.
- Added the requested minimal Geode repro in `RendererGeode_tests.cc` for a left-half path clip over a solid fill.
- Added explicit Geode tests covering:
  - path-clip over a normal fill
  - path-clip over an isolated-layer composite
  - path-clip over a non-rect isolated-layer composite
  - path-clip over a filter-layer composite
- Wired Phase 3b clip-mask bindings through the image-blit pipeline (`GeodeImagePipeline`, `GeodeTextureEncoder`, `image_blit.wgsl`) so layer/filter composites can sample the active clip mask.
- Changed the alpha-coverage clip-mask representation from a single scalar lane to RGBA-packed per-sample coverage, and switched downstream mask reads to exact texel loads instead of filtered UV sampling.
- Stopped clearing the clip stack during `pushFilterLayer()`, so filtered content is captured with the active clip instead of trying to reconstruct the clip only at composite time.

## Validation status

### Passing targeted Geode unit repros

- `RendererGeodeTest.PathClipMaskClipsSolidFillToLeftHalf`
- `RendererGeodeTest.PathClipMaskClipsIsolatedLayerComposite`
- `RendererGeodeTest.PathClipMaskClipsIsolatedLayerCompositeForTriangle`
- `RendererGeodeTest.PathClipMaskClipsFilterLayerCompositeForTriangle`

These were run individually because the Intel Arc + Mesa setup intermittently times out in batches.

### Still failing / incomplete

- `MaskingClipPath/ImageComparisonTestFixture.ResvgTest/simple_case`  
  Still fails with **728 pixels differ**.
- `FiltersFilter/ImageComparisonTestFixture.ResvgTest/with_clip_path`  
  Still hits the per-case timeout in the comparison harness.
- `FiltersFilter/ImageComparisonTestFixture.ResvgTest/with_clip_path_and_mask`  
  Improved substantially, but still fails with **28638 pixels differ**.

## Current conclusion

The original "all-transparent" Phase 3b failure is fixed, and filter-layer clip reapplication is no longer completely broken. The remaining gap is a higher-resolution arbitrary-path coverage mismatch in the alpha-coverage clip-mask path, which still shows up on the resvg star-shaped clip cases.
