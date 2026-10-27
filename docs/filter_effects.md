# SVG Filter Effects {#FilterEffectsGuide}

## Overview

Donner implements all 17 SVG filter primitives from the
[Filter Effects Module Level 1](https://drafts.fxtf.org/filter-effects/) spec, plus CSS shorthand
filter functions (`blur()`, `brightness()`, `drop-shadow()`, etc.). Filters run on both shipping
backends, TinySkia (CPU) and Geode (GPU); the removed full-Skia backend had its own native
lowering path.

**Guarantees:**
- All 17 primitives render correctly with `in`/`result` named buffer routing.
- `filterUnits` (objectBoundingBox, userSpaceOnUse) and `primitiveUnits` are fully supported.
- `color-interpolation-filters` (linearRGB/sRGB) is handled per-primitive.
- CSS shorthand filter functions map to the same filter graph as SVG `<filter>` elements.
- The resvg test suite passes on both backends, excluding documented skips for unimplemented features and known bugs (see the skip list in `donner/svg/renderer/tests/resvg_test_suite.cc`).

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
                                                         └─ Geode: GeodeFilterEngine
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
| Historical full-Skia backend | `origin/skia_archive` | Former native `SkImageFilter` DAG construction |

### Filter Graph Model

```cpp
FilterGraph {
  nodes: vector<FilterNode>     // One per primitive, in document order
  filterRegion: optional<Box2d>  // Computed filter region in user space
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
- Handles linearRGB/sRGB conversion, primitive subregion clipping (rotation-aware through
  `filterFromDevice`), and named buffer routing.

**Historical full-Skia backend** (removed in 2026-04, archived at `origin/skia_archive`):
- Built an `SkImageFilter` tree for all 17 primitives.
- Captured the source graphic into a raster `SkSurface`.
- Applied the filter DAG with rotation-aware region clipping through `SkPath`.
- Fell back to the shared `FilterGraphExecutor` for transformed blur chains and rotated `feTile`.

### feImage Fragment References

`feImage` with `href="#elementId"` renders a same-document element into the filter output:

1. `preRenderFeImageFragments()` creates an offscreen renderer and renders the referenced element.
   Elements in `<defs>` get shadow rendering instances from
   `RenderingContext::createFeImageShadowTree`. A recursion guard (`feImageFragmentGuard_`)
   prevents infinite loops.
2. The fragment is rendered at its natural document position (no surfaceFromCanvasTransform_ offset).
3. The filter pipeline resolves the primitive subregion in user space and positions the content
   at its unclipped origin. Omitted coordinates default individually to the filter region origin;
   primitive/filter clipping does not shift the content. Normal referenced geometry keeps its
   natural dimensions.
   - **No host shear:** TinySkia offsets `targetRect` by the resolved origin converted to pixels.
     Geode encodes the inverse offset in the fragment image shader parameters.
   - **Host has shear/rotation:** The existing fragment transform composes
     `viewBoxScaleInv * Translate(primitiveOrigin) * deviceFromFilter`. The CPU path rasterizes
     through `RasterizeImagePremultiplied`; Geode encodes its inverse in image parameters.

`fragmentRegionTopLeft` on `filter_primitive::Image` carries the fallback filter-region origin.
Explicit or partially specified primitive subregions retain their unclipped resolved coordinates
for placement separately from their pixel-space clipping bounds.

`//donner/svg/renderer/tests:renderer_public_api_tests` enforces exact complete/partial-coordinate,
protrusion and translated-source placement in `FeImageFragmentUsesItsOwnSubregionOrigin`, using
nearest sampling. `//donner/svg/renderer/tests:resvg_test_suite_default_text` enforces the three
`with-x-y` comparisons with ordinary budgets. The configured Geode counterparts are
`//donner/svg/renderer/tests:renderer_public_api_tests_geode` and
`//donner/svg/renderer/tests:resvg_test_suite_geode`; full-text coverage uses the API target with
`--config=text-full` and `//donner/svg/renderer/tests:resvg_test_suite_max`. These bounded cases do
not establish arbitrary host scale, rotation, skew or reflection correctness.

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
void pushFilterLayer(const FilterGraph& filterGraph, const optional<Box2d>& filterRegion);
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
- **Primitive count:** A filter graph holds at most 32 primitives, counting every stage of a
  `filter` property together (each `url()` filter's primitives plus shorthand functions). A
  filter over the limit is never truncated: it fails closed, the element it applies to is not
  rendered, and independent content still renders.
- **`feImage` fetching:** Same-document fragment references use a recursion guard.
  External `href` follows the existing resource loading policy.
- **`feDisplacementMap`:** Cross-origin `in2` produces transparent black per spec.
- **Numeric overflow:** Intermediate pixel values are clamped to [0, 1] at each pipeline stage.
- **Parsing:** Filter attribute parsers are fuzz-tested through the SVG parser fuzzing harness.

## Convolution Bias

For `feConvolveMatrix` with `preserveAlpha="false"`, alpha is the divided convolution sum plus
unscaled `bias`. RGB receives `bias` multiplied by that raw resulting alpha, before clamping.
Stored alpha is clamped to [0, 1] and RGB to [0, stored alpha]. With `preserveAlpha="true"`,
the filter convolves straight RGB,
adds unscaled bias, and premultiplies by the unchanged source alpha.
See [Filter Effects 1, section 9.9](https://www.w3.org/TR/filter-effects-1/#feConvolveMatrixElement).

## Performance Notes

- **Blur:** Uses 3-pass box blur approximation for sigma >= 2.0 (O(w*h) per pass, using a running sum).
  Small sigma uses discrete Gaussian kernel.
- **Morphology:** Currently O(w*h*rx*ry). Could use van Herk/Gil-Werman for O(w*h).
- **ConvolveMatrix:** Full 2D convolution, O(w*h*orderX*orderY). Not separable.
- **Lighting:** Per-pixel normal computation + light vector. Spotlight uses per-pixel cone test.
- **Historical full-Skia backend:** the removed native `SkImageFilter` DAG avoided intermediate
  buffer copies for most graphs.

## Testing and Observability

### Test Targets

| Target | Backend | Description |
|---|---|---|
| `//donner/svg/renderer/tests:resvg_test_suite_default_text` | TinySkia | Full resvg golden image suite |
| `//donner/svg/renderer/tests:renderer_tests` | TinySkia | Deterministic golden tests |
| `//donner/svg/renderer/tests:filter_graph_executor_tests` | TinySkia | Unit tests for executor |
| `//donner/svg/renderer/tests:convolve_matrix_bias_tests` | TinySkia | Literal byte/float convolution bias expectations |
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
primitives. Point/spot light coordinates are scaled from user space to pixel space through
`deviceFromFilter`. Z coordinates use the RMS scale factor.

### RasterizeTransformedImagePremultiplied

Shared utility used in `FilterGraphExecutor.cc` (and previously by the removed full-Skia backend)
for rasterizing images through affine transforms. Used by:
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
  spec behavior; Donner currently uses the SVG default (linearRGB) for the `url()` portion.
