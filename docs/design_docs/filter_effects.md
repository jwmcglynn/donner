# SVG Filter Effects Developer Guide

## Overview

Donner implements all 17 SVG filter primitives from the
[Filter Effects Module Level 1](https://drafts.fxtf.org/filter-effects/) spec, plus CSS shorthand
filter functions (`blur()`, `brightness()`, `drop-shadow()`, etc.). Filters work on both the
TinySkia and Skia rendering backends.

**Key guarantees:**
- All 17 primitives render correctly with `in`/`result` named buffer routing.
- `filterUnits` (objectBoundingBox, userSpaceOnUse) and `primitiveUnits` are fully supported.
- `color-interpolation-filters` (linearRGB/sRGB) is handled per-primitive.
- CSS shorthand filter functions map to the same filter graph as SVG `<filter>` elements.
- The resvg test suite passes on both backends.

**Tracking:** [#151](https://github.com/jwmcglynn/donner/issues/151)

## Architecture Snapshot

### Data Flow

```
SVG DOM                    Filter System              Renderer
---------                  -------------              --------
<filter>                   FilterSystem::             RendererDriver::
  <feBlur>    ──parse──>     buildFilterGraph()  ──>    resolveFilterGraph()
  <feOffset>               FilterGraph {               preRenderFeImageFragments()
  ...                        nodes[]                   renderer.pushFilterLayer()
                             filterRegion              ... render source graphic ...
                             primitiveUnits            renderer.popFilterLayer()
                           }                             ├─ TinySkia: FilterGraphExecutor
                                                         └─ Skia: buildNativeSkiaFilterDAG
```

### Component Layers

| Layer | Location | Responsibility |
|---|---|---|
| SVG element classes | `donner/svg/SVGFe*.h` | DOM API, attribute accessors |
| Attribute parsing | `donner/svg/parser/AttributeParser.cc` | XML attribute -> ECS component |
| ECS components | `donner/svg/components/filter/` | Data storage (`FE*Component`, `FilterGraph`) |
| Filter system | `donner/svg/components/filter/FilterSystem.cc` | DOM -> `FilterGraph` construction |
| Renderer driver | `donner/svg/renderer/RendererDriver.cc` | Orchestrates capture + graph execution |
| TinySkia executor | `donner/svg/renderer/FilterGraphExecutor.cc` | CPU pixmap-based graph execution |
| TinySkia filter lib | `third_party/tiny-skia-cpp/src/tiny_skia/filter/` | Pure pixel math (all primitives) |
| Skia backend | `donner/svg/renderer/RendererSkia.cc` | Native `SkImageFilter` DAG construction |

### Filter Graph Model

```cpp
FilterGraph {
  nodes: vector<FilterNode>     // One per primitive, in document order
  filterRegion: optional<Boxd>  // Computed filter region in user space
  primitiveUnits: PrimitiveUnits
  colorInterpolationFilters: ColorInterpolationFilters
  userToPixelScale: Vector2d    // viewBox -> device pixel scale
}

FilterNode {
  primitive: FilterPrimitive    // Variant of all 17 primitive types
  inputs: vector<FilterInput>   // SourceGraphic, SourceAlpha, Named("foo"), etc.
  result: optional<RcString>    // Named output buffer
  x, y, width, height: optional<Lengthd>  // Primitive subregion overrides
}
```

### Backend Execution

**TinySkia backend** (`FilterGraphExecutor.cc`):
- Allocates `FloatPixmap` buffers for each intermediate result.
- Executes nodes in document order, delegating pixel math to the tiny-skia-cpp filter library.
- Handles linearRGB/sRGB conversion, primitive subregion clipping (rotation-aware via
  `filterFromDevice`), and named buffer routing.

**Skia backend** (`RendererSkia.cc`):
- `buildNativeSkiaFilterDAG()` constructs an `SkImageFilter` tree for all 17 primitives.
- `pushFilterLayer()` captures the source graphic into a raster `SkSurface`.
- `popFilterLayer()` applies the filter DAG and composites with rotation-aware filter region
  clipping via `SkPath`.
- Falls back to the shared `FilterGraphExecutor` for complex cases (transformed blur chains,
  feTile with rotation).

### feImage Fragment References

`feImage` with `href="#elementId"` renders a same-document element into the filter output:

1. `preRenderFeImageFragments()` creates an offscreen renderer and renders the referenced element.
   Elements in `<defs>` get shadow rendering instances via
   `RenderingContext::createFeImageShadowTree`. A recursion guard (`feImageFragmentGuard_`)
   prevents infinite loops.
2. The fragment is rendered at its natural document position (no layerBaseTransform_ offset).
3. The filter pipeline positions the content at the filter region origin:
   - **No host shear:** Device-space offset via `targetRect` (TinySkia) or
     `SkImageFilters::Offset` (Skia).
   - **Host has shear/rotation:** Rasterized through
     `viewBoxScaleInv * Translate(regionTopLeft) * deviceFromFilter` via
     `RasterizeTransformedImagePremultiplied`.

The `fragmentRegionTopLeft` field on `filter_primitive::Image` carries the filter region origin
from pre-rendering to the filter pipeline.

### Color Space Handling

Filter primitives default to `linearRGB` per the `color-interpolation-filters` property.
CSS shorthand functions always use `sRGB`.

The executor wraps each node: convert inputs to the node's color space, execute the primitive,
tag the output. The final output converts back to sRGB before compositing.

Conversion uses LUT-based `srgbToLinear()` / `linearToSrgb()` in the tiny-skia-cpp library.

### Coordinate Space Handling

`primitiveUnits` controls how primitive subregion coordinates and attribute values are interpreted:
- **`userSpaceOnUse` (default):** Values are in the user coordinate system (CSS pixels).
- **`objectBoundingBox`:** Values are fractions of the filtered element's bounding box.

The executor resolves all coordinates to absolute pixel values before invoking filter operations.

Under host rotation/skew, `deviceFromFilter` transforms between filter user space and device
pixels. The filter region is clipped using per-pixel point-in-rect testing against the user-space
rectangle (not the AABB) to produce correct rotated boundaries.

## API Surface

### SVG Elements

Each filter primitive has a corresponding `SVGFe*Element` class (e.g., `SVGFEGaussianBlurElement`).

### Renderer Interface

```cpp
// Push/pop filter layer bracketing source graphic rendering
void pushFilterLayer(const FilterGraph& filterGraph, const optional<Boxd>& filterRegion);
void popFilterLayer();
```

### CSS Filter Property

CSS `filter` property supports both `url()` references and shorthand functions:
```css
filter: blur(5px) brightness(1.2);
filter: url(#myFilter) grayscale(50%);
```

Shorthand functions map to equivalent filter graph nodes internally.

## Security and Safety

- **Resource exhaustion:** Filter region size is bounded (4096px expansion limit). Extreme blur
  radii and convolution kernels are capped.
- **`feImage` fetching:** Same-document fragment references use a recursion guard.
  External `href` follows the existing resource loading policy.
- **`feDisplacementMap`:** Cross-origin `in2` produces transparent black per spec.
- **Numeric overflow:** Intermediate pixel values are clamped to [0, 1] at each pipeline stage.
- **Parsing:** Filter attribute parsers are fuzz-tested via the SVG parser fuzzing harness.

## Performance Notes

- **Blur:** Uses 3-pass box blur approximation for sigma >= 2.0 (O(w*h) per pass via running sum).
  Small sigma uses discrete Gaussian kernel.
- **Morphology:** Currently O(w*h*rx*ry). Could use van Herk/Gil-Werman for O(w*h).
- **ConvolveMatrix:** Full 2D convolution, O(w*h*orderX*orderY). Not separable.
- **Lighting:** Per-pixel normal computation + light vector. Spotlight uses per-pixel cone test.
- **Skia backend:** Native `SkImageFilter` DAG avoids intermediate buffer copies for most graphs.
  CPU fallback path still used for transformed blur and feTile with rotation.

## Testing and Observability

### Test Targets

| Target | Backend | Description |
|---|---|---|
| `//donner/svg/renderer/tests:resvg_test_suite_tiny_skia` | TinySkia | Full resvg golden image suite |
| `//donner/svg/renderer/tests:resvg_test_suite_skia` | Skia | Same suite, Skia backend |
| `//donner/svg/renderer/tests:renderer_tests_tiny_skia` | TinySkia | Deterministic golden tests |
| `//donner/svg/renderer/tests:renderer_tests_skia` | Skia | Same tests, Skia backend |
| `//donner/svg/renderer/tests:filter_graph_executor_tests` | TinySkia | Unit tests for executor |
| `third_party/tiny-skia-cpp/...` | N/A | Per-primitive pixel math unit tests |

### Adding Filter Tests

1. Add an SVG fixture in `donner/svg/renderer/testdata/` or use the resvg test suite.
2. For resvg tests, add an `INSTANTIATE_TEST_SUITE_P` entry in `resvg_test_suite.cc`.
3. Tests that differ by <100 pixels at threshold 0.01 need no override entry.
4. Use `Params::WithThreshold()` for expected rendering differences (e.g., bilinear interpolation).
5. Use `Params::Skip()` only for UB cases or unimplemented features (requires approval).

### Verbose Debugging

Set `DONNER_RENDERER_TEST_VERBOSE=1` and `LLM=0` when running tests to get per-pixel diff output,
transform traces, and filter pipeline debug logging.

## Per-Primitive Reference

### Primitive Summary

| Primitive | tiny-skia-cpp API | Skia API | Notes |
|---|---|---|---|
| `feGaussianBlur` | `filter::gaussianBlur` | `SkImageFilters::Blur` | 3-pass box blur approx |
| `feFlood` | `filter::flood` | `SkImageFilters::Shader` | Solid color fill |
| `feOffset` | `filter::offset` | `SkImageFilters::Offset` | Integer + bilinear subpixel |
| `feColorMatrix` | `filter::colorMatrix` | `SkColorFilters::Matrix` | matrix/saturate/hueRotate/luminanceToAlpha |
| `feComposite` | `filter::composite` | `SkImageFilters::Blend`/`Arithmetic` | Porter-Duff + arithmetic |
| `feBlend` | `filter::blend` | `SkImageFilters::Blend` | CSS blend modes |
| `feComponentTransfer` | `filter::componentTransfer` | CPU rasterization | LUT-based per-channel |
| `feDropShadow` | decomposed | decomposed | flood+composite+offset+blur+merge |
| `feMorphology` | `filter::morphology` | `SkImageFilters::Dilate`/`Erode` | erode/dilate |
| `feConvolveMatrix` | `filter::convolveMatrix` | CPU rasterization | Full 2D convolution |
| `feTile` | `filter::tile` | CPU rasterization | Modular tiling |
| `feTurbulence` | `filter::turbulence` | CPU rasterization | SVG-spec Perlin noise |
| `feImage` | renderer-only | renderer-only | External image / fragment ref |
| `feDisplacementMap` | `filter::displacementMap` | CPU rasterization | Channel-based displacement |
| `feDiffuseLighting` | `filter::diffuseLighting` | CPU rasterization | SVG-spec spotlight |
| `feSpecularLighting` | `filter::specularLighting` | CPU rasterization | Phong model |

Light sources (`feDistantLight`, `fePointLight`, `feSpotLight`) are children of lighting
primitives. Point/spot light coordinates are scaled from user space to pixel space via
`deviceFromFilter`. Z coordinates use the RMS scale factor.

### RasterizeTransformedImagePremultiplied

Shared utility (in both `FilterGraphExecutor.cc` and `RendererSkia.cc`) for rasterizing images
through affine transforms. Used by:
- `feImage` external images with host rotation/skew
- `feImage` fragment references with host shear

Uses bilinear interpolation with pixel-center sampling. Sample centers outside the source image
produce transparent pixels (no edge clamping) to prevent images from bleeding beyond their
`preserveAspectRatio` placement rectangle.

## Limitations and Future Extensions

- `feImage` fragment references render through an 8-bit intermediate buffer; rendering directly
  into the float filter buffer would eliminate minor edge-pixel differences.
- `BackgroundImage` / `BackgroundAlpha` standard inputs are not implemented (require CSS
  compositing isolation groups).
- Filter application on the root `<svg>` element is not supported.
- CSS `backdrop-filter` is not supported.
- Filter parameter animation is not supported (separate animation milestone).
- Color space in mixed `url()` + CSS filter chains (`filter: url(#f) grayscale()`) has ambiguous
  spec behavior — currently uses the SVG default (linearRGB) for the `url()` portion.
