# Design: Renderer Abstraction Layer

**Status:** Design
**Author:** Claude Opus 4.6
**Created:** 2026-02-19

## Summary

Introduce a backend-agnostic rendering abstraction for Donner that decouples the SVG render-tree
traversal from the graphics API. Today, `RendererSkia::Impl` interleaves tree walking (deciding
*what* to draw) with Skia-specific calls (deciding *how* to draw). This design splits the renderer
into three layers:

| Layer | Responsibility |
|-------|---------------|
| **`RendererInterface`** | Abstract canvas-like API using Donner types. No Skia dependency. |
| **`RendererDriver`** | Walks `RenderingInstanceComponent`s, resolves paints/transforms/masks, emits calls to `RendererInterface`. |
| **`RendererSkia`** | Implements `RendererInterface` by translating each call to the corresponding Skia API. |

This enables a second backend (the caller's custom renderer) to be plugged in without duplicating
the ~800 lines of tree-traversal and paint-resolution logic that currently live inside
`RendererSkia::Impl`.

## Goals

- Define a `RendererInterface` with a canvas-like imperative API, expressed entirely in Donner
  types (`Transformd`, `PathSpline`, `css::RGBA`, etc.) with zero Skia includes.
- Extract the render-tree traversal and paint/style resolution into `RendererDriver`, which calls
  `RendererInterface` methods.
- Reimplement the existing Skia rendering path as `RendererSkia : RendererInterface` with no
  behavioral change (pixel-identical output for existing tests).
- Make it straightforward for an external consumer to implement `RendererInterface` for a custom
  backend.

## Non-Goals

- GPU acceleration or render-graph batching — backends own their own execution model.
- Changing the ECS render-tree instantiation (`RenderingContext`, `RenderingInstanceComponent`).
  The driver consumes these as-is.
- Text layout engine — `RendererInterface::drawText` receives pre-positioned spans. Font loading
  and shaping remain backend responsibilities.
- Adding new SVG features (e.g., full filter graph). The interface should be *extensible* toward
  these, but this design only covers currently-implemented features.
- Thread safety. The renderer is single-threaded as it is today.

## Next Steps

1. Introduce the `RendererInterface` header and the Donner-native paint/style types in
   `donner/svg/renderer/`.
2. Extract `RendererDriver` from `RendererSkia::Impl`, keeping `RendererSkia` as the backend.
3. Validate pixel-identical output against existing golden tests.

## Implementation Plan

- [ ] **Milestone 1: Define `RendererInterface` and supporting types**
  - [ ] Create `donner/svg/renderer/RendererInterface.h` with the abstract class.
  - [ ] Create `donner/svg/renderer/RendererPaintTypes.h` with `SolidPaint`,
        `LinearGradientPaint`, `RadialGradientPaint`, `PatternPaint`, `FillStyle`, `StrokeStyle`,
        `TextStyle`, and `LayerPaint`.
  - [ ] Create `donner/svg/renderer/Recording.h` with the `Recording` base class and
        `RecordingHandle` typedef.
  - [ ] Add BUILD target `//donner/svg/renderer:renderer_interface` with no Skia dependency.
- [ ] **Milestone 2: Extract `RendererDriver`**
  - [ ] Create `donner/svg/renderer/RendererDriver.h/.cc`.
  - [ ] Move tree-traversal logic from `RendererSkia::Impl::drawUntil` into
        `RendererDriver::renderUntil`.
  - [ ] Move paint resolution (`instantiateGradient`, `instantiatePattern`,
        `instantiatePaintReference`) into `RendererDriver`, translating resolved ECS components
        into the Donner-native paint types.
  - [ ] Move mask, clip-path, filter, marker, and subtree management into `RendererDriver`.
  - [ ] The driver calls `RendererInterface` methods exclusively.
- [ ] **Milestone 3: Implement `RendererSkia` against `RendererInterface`**
  - [ ] Refactor `RendererSkia` to implement `RendererInterface`.
  - [ ] Move Skia-specific conversion helpers (`toSkia(...)`, `toSkiaMatrix(...)`) into
        `RendererSkia.cc` as private utilities.
  - [ ] Font management stays in `RendererSkia` (backend-specific).
  - [ ] `RendererSkia::draw(SVGDocument&)` creates a `RendererDriver` internally.
- [ ] **Milestone 4: Validation**
  - [ ] All existing renderer golden tests pass with identical output.
  - [ ] Add a `MockRenderer : RendererInterface` for unit-testing `RendererDriver` in isolation.
  - [ ] Test that an empty/no-op `RendererInterface` implementation compiles and links.

## Background

### Current architecture

```
SVGDocument
    │
    ▼
RendererUtils::prepareDocumentForRendering()
    │  Runs ECS systems: Style → Layout → Shape → Text →
    │  ShadowTree → Paint → Filter → RenderingContext
    ▼
Registry contains sorted RenderingInstanceComponents
    │
    ▼
RendererSkia::Impl::drawUntil()
    │  Walks instances in draw-order.
    │  For each instance:
    │    - Sets transform on SkCanvas
    │    - Handles clip-rect, clip-path, mask, opacity, filter layers
    │    - Resolves paint (solid / gradient / pattern)
    │    - Calls SkCanvas draw methods
    ▼
SkBitmap (pixels)
```

`RendererSkia::Impl` (~1100 lines in `RendererSkia.cc`) mixes two concerns:

1. **Traversal & resolution** — iterating `RenderingInstanceComponent`s, resolving
   `ResolvedPaintServer` variants, managing subtree markers, computing gradient/pattern transforms.
2. **Backend calls** — `SkCanvas::save/restore/saveLayer/drawPath/drawImage`, `SkPaint`
   configuration, `SkGradientShader` creation, `SkPictureRecorder` for patterns.

Separating these lets a second backend reuse all of (1).

### Skia API surface currently used

| Category | Skia calls |
|----------|-----------|
| State | `save`, `restore`, `restoreToCount`, `saveLayer` |
| Transform | `setMatrix`, `concat`, `resetMatrix`, `translate`, `rotate` |
| Clip | `clipRect`, `clipPath` |
| Draw | `drawPath`, `drawImage`, `drawSimpleText` |
| Paint | `setColor`, `setAlphaf`, `setStyle`, `setAntiAlias`, `setStrokeWidth`, `setStrokeCap`, `setStrokeJoin`, `setStrokeMiter`, `setShader`, `setPathEffect`, `setColorFilter`, `setBlendMode`, `setImageFilter` |
| Shader | `SkGradientShader::MakeLinear`, `MakeRadial`, `MakeTwoPointConical`, `SkPicture::makeShader` |
| Effect | `SkDashPathEffect::Make`, `SkImageFilters::Blur`, `SkLumaColorFilter::Make` |
| Recording | `SkPictureRecorder::beginRecording`, `finishRecordingAsPicture` |

## Proposed Architecture

### Layer diagram

```
┌──────────────────────────────────────────────────────────┐
│                     User Code                            │
│  auto renderer = RendererSkia();       // or MyBackend() │
│  RendererDriver driver(renderer);                        │
│  driver.render(document);                                │
└──────────────┬───────────────────────────────────────────┘
               │
               │ calls RendererInterface methods
               ▼
┌──────────────────────────────────────────────────────────┐
│                   RendererDriver                         │
│                                                          │
│  Walks RenderingInstanceView (sorted by drawOrder).      │
│  For each instance:                                      │
│    1. Computes entityFromCanvas transform                │
│    2. Manages canvas state (save/restore/saveLayer)      │
│    3. Applies clip-rect, clip-path, mask, filter layers  │
│    4. Resolves ResolvedPaintServer → Paint types         │
│    5. Calls renderer.fillPath / strokePath / drawImage   │
│    6. Handles subtree rendering (patterns, masks,        │
│       markers) recursively                               │
│                                                          │
│  Owns: RenderingInstanceView, subtreeMarkers_ stack,     │
│        layerBaseTransform_                               │
│  Depends on: ECS components (read-only), RendererInterface│
└──────────────┬───────────────────────────────────────────┘
               │
               │ virtual calls
               ▼
┌──────────────────────────────────────────────────────────┐
│               RendererInterface (abstract)                │
│                                                          │
│  Canvas-like API using Donner types only.                │
│  No ECS, no Skia, no tree knowledge.                     │
│                                                          │
│  save / restore / saveLayer                              │
│  setTransform / concatTransform                          │
│  clipRect / clipPath                                     │
│  fillPath / strokePath / drawImage / drawText            │
│  beginRecording / endRecording                           │
└──────────────┬───────────────────────────────────────────┘
               │
      ┌────────┴────────┐
      ▼                 ▼
┌───────────┐   ┌───────────────┐
│RendererSkia│   │CustomBackend  │
│(Skia impl) │   │ (user's impl) │
└───────────┘   └───────────────┘
```

### Data flow for a filled rect with gradient paint

```
RendererDriver                          RendererInterface (Skia)
─────────────                           ────────────────────────
1. save()                           →   SkCanvas::save()
2. setTransform(entityFromCanvas)   →   SkCanvas::setMatrix(toSkiaMatrix(...))
3. fillPath(                        →   SkPath skPath = toSkia(path)
     path = rectPathSpline,             SkPaint paint
     FillStyle {                        paint.setShader(SkGradientShader::MakeLinear(
       paint = LinearGradientPaint{         toSkia(start), toSkia(end),
         start, end,                        colors, positions, ...
         stops, spreadMethod,               &skMatrix))
         gradientTransform              paint.setStyle(kFill)
       },                              paint.setFillType(kWinding)
       fillRule = NonZero               canvas->drawPath(skPath, paint)
     }
   )
4. restore()                        →   SkCanvas::restore()
```

### Data flow for pattern fill

```
RendererDriver                          RendererInterface (Skia)
─────────────                           ────────────────────────
1. beginRecording(tileRect)         →   SkPictureRecorder::beginRecording(skRect)
                                        Push new canvas onto canvas stack
2. [render pattern subtree]         →   [normal draw calls go to recorder canvas]
   setTransform(...)                    SkCanvas::setMatrix(...)
   fillPath(...)                        SkCanvas::drawPath(...)
   ...                                  ...
3. endRecording() → RecordingHandle →   SkPictureRecorder::finishRecordingAsPicture()
                                        Pop canvas stack
                                        Return SkiaRecording{sk_sp<SkPicture>}
4. fillPath(path, FillStyle {       →   auto& rec = static_cast<SkiaRecording&>(*handle)
     paint = PatternPaint {             SkPaint paint
       recording = handle,              paint.setShader(rec.picture->makeShader(
       tileRect, transform                  kRepeat, kRepeat, &matrix, &rect))
     },                                 canvas->drawPath(skPath, paint)
     fillRule
   })
```

## API / Interfaces

### Paint types (`RendererPaintTypes.h`)

All paint types use Donner types exclusively. Colors are fully resolved `css::RGBA` values (the
driver resolves `currentColor` and applies opacity before passing to the renderer).

```cpp
namespace donner::svg {

/// A gradient color stop with a fully resolved color.
struct ColorStop {
  float offset;    ///< Position in [0, 1].
  css::RGBA color; ///< Fully resolved RGBA color.
};

/// Solid color paint.
struct SolidPaint {
  css::RGBA color;
};

/// Linear gradient paint with fully resolved parameters.
struct LinearGradientPaint {
  Vector2d start;                        ///< Gradient start point.
  Vector2d end;                          ///< Gradient end point.
  std::vector<ColorStop> stops;          ///< Color stops.
  GradientSpreadMethod spreadMethod;     ///< Edge behavior.
  Transformd gradientTransform;          ///< Gradient coordinate transform.
};

/// Radial gradient paint with fully resolved parameters.
struct RadialGradientPaint {
  Vector2d center;                       ///< End circle center.
  double radius;                         ///< End circle radius.
  Vector2d focalCenter;                  ///< Start (focal) circle center.
  double focalRadius;                    ///< Start (focal) circle radius.
  std::vector<ColorStop> stops;          ///< Color stops.
  GradientSpreadMethod spreadMethod;     ///< Edge behavior.
  Transformd gradientTransform;          ///< Gradient coordinate transform.
};

/// Pattern paint using a previously recorded tile.
struct PatternPaint {
  /// Opaque handle to recorded content (from endRecording()).
  /// Ownership is shared since the same pattern may be referenced
  /// by both fill and stroke of the same element.
  std::shared_ptr<Recording> recording;
  Boxd tileRect;                         ///< Tile bounds in pattern space.
  Transformd patternTransform;           ///< Pattern-to-user-space transform.
};

/// Union of all paint types that can be passed to fill/stroke calls.
using Paint = std::variant<SolidPaint, LinearGradientPaint, RadialGradientPaint, PatternPaint>;
```

### Style types

```cpp
/// Parameters for a dash pattern.
struct DashParams {
  std::vector<double> intervals; ///< Alternating dash/gap lengths in user units.
  double offset = 0.0;          ///< Starting offset into the dash pattern.
};

/// Fill style passed to fillPath().
struct FillStyle {
  Paint paint;
  FillRule fillRule = FillRule::NonZero;
};

/// Stroke style passed to strokePath().
struct StrokeStyle {
  Paint paint;
  double width = 1.0;
  StrokeLinecap linecap = StrokeLinecap::Butt;
  StrokeLinejoin linejoin = StrokeLinejoin::Miter;
  double miterLimit = 4.0;
  std::optional<DashParams> dash;
};

/// Text style passed to drawText().
struct TextStyle {
  css::RGBA color;
  std::string fontFamily;
  double fontSize = 16.0;
};

/// Image data passed to drawImage(). Contains raw RGBA pixel data.
struct ImageData {
  int width;
  int height;
  std::span<const uint8_t> data; ///< RGBA pixel data, size = width * height * 4.
};
```

### Layer types

```cpp
/// Resolved image filter for the renderer (no element references).
struct ImageFilter {
  struct Blur {
    double stdDeviationX;
    double stdDeviationY;
  };

  using Type = std::variant<Blur>;
  Type value;
};

/// Parameters for saveLayer(). Fields are combined — set only what is needed.
struct LayerPaint {
  float opacity = 1.0f;                         ///< Layer opacity (1.0 = opaque).
  std::optional<BlendMode> blendMode;            ///< Compositing blend mode.
  bool lumaToAlpha = false;                      ///< Apply luminance-to-alpha color filter.
  std::optional<ImageFilter> imageFilter;        ///< Image filter (e.g., blur).
};

/// Blend modes used for mask compositing and isolated layers.
enum class BlendMode {
  SrcOver,   ///< Default: source over destination.
  SrcIn,     ///< Source where destination has alpha (for masks).
  // Extensible for future SVG compositing modes.
};
```

### Recording types (`Recording.h`)

```cpp
/// Base class for backend-specific recorded content (pattern tiles, etc.).
/// Backends subclass this to store their recorded representation.
class Recording {
public:
  virtual ~Recording() = default;
};

/// Handle to recorded content. Shared ownership because a pattern recording
/// may be referenced by both fill and stroke of the same shape.
using RecordingHandle = std::shared_ptr<Recording>;
```

### `RendererInterface` (`RendererInterface.h`)

```cpp
namespace donner::svg {

/// Abstract rendering interface with a canvas-like API.
///
/// All coordinates are in the current transform space. The transform is
/// managed via setTransform/concatTransform. State is managed via
/// save/restore (the same model as SkCanvas and HTML Canvas).
///
/// All inputs use Donner types — no backend types leak into this interface.
class RendererInterface {
public:
  virtual ~RendererInterface() = default;

  //--------------------------------------------------------------------
  // Configuration
  //--------------------------------------------------------------------

  /// Enable or disable antialiasing for subsequent draw calls.
  virtual void setAntialias(bool antialias) = 0;

  //--------------------------------------------------------------------
  // Canvas state
  //--------------------------------------------------------------------

  /// Save the current canvas state (transform, clip).
  virtual void save() = 0;

  /// Restore the most recently saved canvas state.
  virtual void restore() = 0;

  /// Return the current save count (number of states on the stack).
  virtual int saveCount() const = 0;

  /// Restore to a specific save count, popping all states above it.
  virtual void restoreToCount(int count) = 0;

  //--------------------------------------------------------------------
  // Isolated layers
  //--------------------------------------------------------------------

  /// Save a new compositing layer. Content drawn into this layer is
  /// flattened into the parent when restore() is called.
  ///
  /// @param bounds  Optional clip bounds for the layer (optimization hint).
  /// @param paint   Layer parameters (opacity, blend mode, filters).
  virtual void saveLayer(const std::optional<Boxd>& bounds,
                         const LayerPaint& paint) = 0;

  //--------------------------------------------------------------------
  // Transform
  //--------------------------------------------------------------------

  /// Replace the current transform with the given transform.
  virtual void setTransform(const Transformd& transform) = 0;

  /// Post-multiply the current transform by the given transform.
  virtual void concatTransform(const Transformd& transform) = 0;

  /// Reset the current transform to identity.
  virtual void resetTransform() = 0;

  //--------------------------------------------------------------------
  // Clipping
  //--------------------------------------------------------------------

  /// Intersect the current clip with the given rectangle.
  virtual void clipRect(const Boxd& rect, bool antialias = true) = 0;

  /// Intersect the current clip with the given path.
  virtual void clipPath(const PathSpline& path, FillRule fillRule,
                        bool antialias = true) = 0;

  //--------------------------------------------------------------------
  // Drawing
  //--------------------------------------------------------------------

  /// Fill the given path with the specified fill style.
  virtual void fillPath(const PathSpline& path,
                        const FillStyle& style) = 0;

  /// Stroke the given path with the specified stroke style.
  virtual void strokePath(const PathSpline& path,
                          const StrokeStyle& style) = 0;

  /// Draw an image into the given destination rectangle, clipped to
  /// clipRect if set.
  ///
  /// @param image      Raw pixel data.
  /// @param destRect   Destination rectangle in current coordinates.
  /// @param clipRect   Optional clip rectangle applied before drawing.
  virtual void drawImage(const ImageData& image,
                         const Boxd& destRect,
                         const std::optional<Boxd>& clipRect = std::nullopt) = 0;

  /// Draw a text string at the given position.
  ///
  /// Font loading and shaping are backend responsibilities. The renderer
  /// should use the TextStyle to select the appropriate font.
  virtual void drawText(std::string_view text, const Vector2d& position,
                        const TextStyle& style) = 0;

  //--------------------------------------------------------------------
  // Offscreen recording (for patterns)
  //--------------------------------------------------------------------

  /// Begin recording draw calls into an offscreen surface.
  /// All subsequent draw calls target the recording until endRecording()
  /// is called. Recordings can be nested (for patterns-within-patterns).
  ///
  /// @param bounds  Bounds of the recording surface.
  virtual void beginRecording(const Boxd& bounds) = 0;

  /// Finish recording and return a handle to the recorded content.
  /// Subsequent draw calls target the previous surface.
  virtual RecordingHandle endRecording() = 0;
};

}  // namespace donner::svg
```

### `RendererDriver` (`RendererDriver.h`)

```cpp
namespace donner::svg {

/// Drives the rendering of an SVG document by walking the render tree
/// and emitting calls to a RendererInterface.
///
/// This class contains all ECS-aware logic: resolving paint servers,
/// computing transforms, managing subtree markers, handling masks,
/// clip-paths, filters, and markers. The RendererInterface implementation
/// does not need to know about ECS or the SVG DOM.
class RendererDriver {
public:
  /// Construct a driver that renders into the given backend.
  ///
  /// @param renderer  The rendering backend to emit draw calls to.
  /// @param verbose   Enable verbose logging to stdout.
  explicit RendererDriver(RendererInterface& renderer, bool verbose = false);

  ~RendererDriver();

  // Non-copyable, non-moveable (holds reference to renderer).
  RendererDriver(const RendererDriver&) = delete;
  RendererDriver& operator=(const RendererDriver&) = delete;

  /// Render the full document. Calls RendererUtils::prepareDocumentForRendering
  /// internally, then walks the rendering instances.
  void render(SVGDocument& document);

  /// Render from the registry directly (assumes prepareDocumentForRendering
  /// was already called).
  void render(Registry& registry);

private:
  /// Internal implementation (pimpl) to keep ECS headers out of the public API.
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace donner::svg
```

### Refactored `RendererSkia` (`RendererSkia.h`)

```cpp
namespace donner::svg {

/// Skia-based implementation of RendererInterface.
///
/// Also provides convenience methods for rendering to bitmaps, PNG files,
/// ASCII art, and SkPictures, which create a RendererDriver internally.
class RendererSkia : public RendererInterface {
public:
  explicit RendererSkia(bool verbose = false);
  ~RendererSkia() override;

  RendererSkia(RendererSkia&&) noexcept;
  RendererSkia& operator=(RendererSkia&&) noexcept;
  RendererSkia(const RendererSkia&) = delete;
  RendererSkia& operator=(const RendererSkia&) = delete;

  //--- Convenience API (creates RendererDriver internally) ---------------

  /// Render the document to the internal bitmap.
  void draw(SVGDocument& document);

  /// Render into ASCII art.
  std::string drawIntoAscii(SVGDocument& document);

  /// Render into an SkPicture.
  sk_sp<SkPicture> drawIntoSkPicture(SVGDocument& document);

  /// Save the internal bitmap to PNG.
  bool save(const char* filename);

  /// Get raw pixel data (RGBA).
  std::span<const uint8_t> pixelData() const;

  int width() const;
  int height() const;
  const SkBitmap& bitmap() const;

  //--- RendererInterface implementation ----------------------------------

  void setAntialias(bool antialias) override;

  void save() override;
  void restore() override;
  int saveCount() const override;
  void restoreToCount(int count) override;

  void saveLayer(const std::optional<Boxd>& bounds,
                 const LayerPaint& paint) override;

  void setTransform(const Transformd& transform) override;
  void concatTransform(const Transformd& transform) override;
  void resetTransform() override;

  void clipRect(const Boxd& rect, bool antialias = true) override;
  void clipPath(const PathSpline& path, FillRule fillRule,
                bool antialias = true) override;

  void fillPath(const PathSpline& path,
                const FillStyle& style) override;
  void strokePath(const PathSpline& path,
                  const StrokeStyle& style) override;

  void drawImage(const ImageData& image, const Boxd& destRect,
                 const std::optional<Boxd>& clipRect = std::nullopt) override;
  void drawText(std::string_view text, const Vector2d& position,
                const TextStyle& style) override;

  void beginRecording(const Boxd& bounds) override;
  RecordingHandle endRecording() override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace donner::svg
```

## How `RendererDriver` maps to current `RendererSkia::Impl`

The following table shows where each block of logic in `RendererSkia::Impl` moves:

| Current location | New location | Notes |
|-----------------|-------------|-------|
| `drawUntil()` loop: instance iteration, subtree markers, restore management | `RendererDriver::Impl` | Core traversal loop |
| `drawUntil()`: `clipRect` handling | `RendererDriver` → calls `renderer.save()` + `renderer.clipRect()` | |
| `drawUntil()`: `setMatrix(entityFromCanvas)` | `RendererDriver` → calls `renderer.setTransform()` | Driver computes `entityFromCanvas = layerBase * entityFromWorld` |
| `drawUntil()`: opacity layer | `RendererDriver` → calls `renderer.saveLayer({}, {.opacity = ...})` | |
| `drawUntil()`: filter layer | `RendererDriver` → resolves filter chain → calls `renderer.saveLayer({}, {.imageFilter = ...})` | |
| `drawUntil()`: clip-path computation | `RendererDriver` → computes unified clip path from `ComputedClipPathsComponent` → calls `renderer.clipPath()` | Path boolean ops (union/intersect) move to driver since they use `PathSpline` ops, not Skia path ops. Requires adding path boolean ops to `PathSpline` or a utility. |
| `drawUntil()`: mask setup | `RendererDriver` → calls `renderer.saveLayer({.lumaToAlpha = true})`, renders mask, then `renderer.saveLayer({.blendMode = SrcIn})` | |
| `drawPath()` | `RendererDriver` → dispatches to `fillPath()` / `strokePath()` / `drawMarkers()` | |
| `drawPathFill()` + `drawPathFillWithSkPaint()` | `RendererDriver` resolves paint → calls `renderer.fillPath(path, fillStyle)` | |
| `drawPathStroke()` + `drawPathStrokeWithSkPaint()` | `RendererDriver` resolves paint → calls `renderer.strokePath(path, strokeStyle)` | |
| `instantiateGradient()` | `RendererDriver` → builds `LinearGradientPaint` or `RadialGradientPaint` from `ComputedGradientComponent` + `ComputedLinear/RadialGradientComponent` | All gradient math (unit resolution, objectBoundingBox transform) stays in driver |
| `instantiatePattern()` | `RendererDriver` → calls `renderer.beginRecording()`, renders subtree, calls `renderer.endRecording()` → builds `PatternPaint` | |
| `instantiateMask()` | `RendererDriver` → issues saveLayer/draw/saveLayer sequence | |
| `drawImage()` | `RendererDriver` → calls `renderer.drawImage()` | |
| `drawText()` | `RendererDriver` → calls `renderer.drawText()` for each span | |
| `drawMarkers()` + `drawMarker()` | `RendererDriver` → computes marker transforms, calls renderer for each marker instance | |
| `createFilterChain()` + `createFilterPaint()` | `RendererDriver` → resolves to `ImageFilter` type, passes to `saveLayer` | |
| `toSkia(PathSpline)` | `RendererSkia` (private) | Backend-specific conversion |
| `toSkia(css::RGBA)` | `RendererSkia` (private) | Backend-specific conversion |
| `toSkiaMatrix(Transformd)` | `RendererSkia` (private) | Backend-specific conversion |
| Font loading, typeface management | `RendererSkia` (private) | Backend-specific |

## Design Decisions

### Why a virtual interface rather than templates?

- Backend selection is a deployment-time decision, not a hot-path per-frame decision. Virtual
  dispatch cost is negligible compared to actual draw work.
- Virtual interfaces provide a clear compilation boundary — `RendererDriver` can be compiled once
  and linked against any backend.
- Backends can be loaded dynamically (e.g., shared library plugins) if needed in the future.

### Why `Paint` as a variant rather than opaque handles?

An alternative is to have the renderer expose opaque `PaintHandle` objects created by factory
methods (`createSolidPaint(...)`, `createGradientPaint(...)`, etc.). We rejected this because:

- **Variant is simpler.** The driver builds a `FillStyle` in one place and passes it. No lifetime
  management for handles.
- **Paint data is small.** Gradient stops are a handful of entries. Copying is cheap.
- **Pattern recording is the exception**, handled via `RecordingHandle` (shared_ptr) inside the
  `PatternPaint` variant member.

### Why does the driver handle path boolean ops for clip-paths?

The current code uses `SkPathOps` (Skia path boolean operations) to union/intersect clip paths.
This is problematic for backend independence. Options:

1. **Add path boolean ops to `PathSpline`** — requires implementing or wrapping a path ops
   library. Clean but significant work.
2. **Pass individual clip paths to the renderer** — the renderer applies them sequentially with
   intersect. Simpler but may not match Skia's behavior for layered clip-path unions.
3. **Expose a `pathOps()` method on `RendererInterface`** — backends provide their own path ops
   implementation. Pragmatic.

Recommended: **Option 3** initially, with a `clipPathCombined()` method on `RendererInterface`
that takes a list of `(PathSpline, FillRule, Transformd)` tuples and the backend computes the
combined clip. This pushes the path-ops requirement to the backend, where it's likely already
available (Skia has it; other backends typically do too).

```cpp
struct ClipPathEntry {
  PathSpline path;
  FillRule clipRule;
  Transformd transform;
  int layer;  ///< Nesting depth for intersect grouping.
};

/// Apply a combined clip path (union within layers, intersect between layers).
virtual void clipPathCombined(std::span<const ClipPathEntry> entries,
                              bool antialias = true) = 0;
```

### Font and text rendering strategy

Text is fully backend-owned. The driver passes:
- Pre-positioned text spans (x, y, dx, dy from `ComputedTextComponent`)
- Font family name and size
- Fill color

The backend loads fonts using its own font system (Skia FreeType, CoreText, or a custom stack).
This keeps the interface clean at the cost of requiring each backend to handle fonts.

## Requirements and Constraints

- **Zero Skia includes** in `RendererInterface.h`, `RendererPaintTypes.h`, `Recording.h`, and
  `RendererDriver.h/.cc`.
- **Pixel-identical output** for all existing golden tests after refactoring.
- **No new allocations in the hot path** beyond what exists today. `std::vector<ColorStop>` in
  gradient paints may allocate, but the current code allocates `std::vector<SkScalar>` and
  `std::vector<SkColor>` in the same places.
- **No breaking changes** to the public `RendererSkia` API. The convenience methods (`draw`,
  `save`, `pixelData`, `drawIntoAscii`) must continue to work.
- **Bazel build**: `RendererInterface` and `RendererDriver` must not depend on the Skia build
  target. `RendererSkia` depends on both.

## Performance

The refactoring is expected to be performance-neutral:

- Virtual dispatch adds one indirection per draw call. There are O(N) calls where N = number of
  visible elements + their paint operations. This is dominated by actual GPU/rasterization work.
- Paint types are passed by const-reference. `PatternPaint` uses `shared_ptr` for the recording
  handle, which is created once per pattern.
- No additional copies of `PathSpline` — passed by const-reference throughout.
- The `toSkia(PathSpline)` conversion in `RendererSkia` reconstructs the SkPath each time, same
  as today. Future optimization: cache the SkPath alongside the PathSpline.

## Testing and Validation

- **Golden tests**: The existing renderer test suite compares output against reference PNGs. These
  must pass identically after the refactoring (byte-identical bitmaps).
- **RendererDriver unit tests**: A `MockRenderer : RendererInterface` records all calls. Tests
  verify that for a given SVG input, the driver emits the expected sequence of
  `setTransform` / `fillPath` / `saveLayer` / etc. calls with correct parameters.
- **Interface compilation test**: A no-op `NullRenderer : RendererInterface` that implements all
  methods as empty bodies. Verifies the interface compiles without Skia.
- **Pattern/mask/clip-path integration tests**: Specific SVGs exercising each complex feature,
  verified through golden comparison.

## Alternatives Considered

### Single monolithic renderer with backend callbacks

Instead of a full interface, inject only the low-level draw operations (drawPath, drawImage) as
callbacks, keeping save/restore/clip/transform management in the driver. Rejected because
backends may have different state management models, and `saveLayer` behavior is fundamentally
tied to the backend's compositing model.

### Intermediate representation (display list / command buffer)

Record all draw commands into a serializable command buffer, then replay on the backend. This
adds a serialization/deserialization step and makes pattern recording awkward (commands within
commands). Adds complexity without clear benefit for the current single-threaded use case.
Could be revisited for multi-threaded rendering in the future.

### Move paint resolution into the renderer backend

Let the backend resolve `ResolvedPaintServer` variants directly. This would require the backend
to understand ECS components and gradient/pattern semantics, defeating the purpose of the
abstraction. The whole point is that backends only need to understand simple draw primitives.

## Open Questions

1. **Path boolean operations**: Should `PathSpline` gain native boolean ops (union, intersect),
   or should the `clipPathCombined` approach described above be used? If PathSpline grows boolean
   ops, which library should back it (Clipper2, custom implementation)?

2. **Font registration**: Should `RendererInterface` have a `registerFont(data)` method so the
   driver can pass loaded WOFF/OTF data to backends? Currently font loading is entirely in
   `RendererSkia::Impl::initialize()`.

3. **Filter extensibility**: When the full SVG filter graph is implemented (feColorMatrix,
   feComposite, feMerge, etc.), should filters become a separate `FilterInterface`, or should
   `RendererInterface::saveLayer` grow to accept a filter graph description?

## Future Work

- [ ] Implement a `RendererNull` for headless testing and benchmarking.
- [ ] Add a display-list/command-buffer `RendererInterface` implementation that serializes draw
      calls for deferred execution or cross-thread replay.
- [ ] Extended filter support (feColorMatrix, feComposite, feBlend, etc.) as new `ImageFilter`
      variants.
- [ ] GPU-accelerated backend example (e.g., wgpu, Vulkan).
- [ ] Text layout integration — move text shaping/positioning before the renderer so the
      interface receives glyph runs rather than raw strings.
