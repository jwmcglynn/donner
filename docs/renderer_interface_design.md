# Renderer Interface Refactor Design

## Goals
- Introduce an abstract renderer interface to decouple traversal logic from backend-specific
  drawing.
- Allow multiple rendering backends (e.g., Skia, headless testing) to coexist behind a
  consistent API.
- Preserve existing `SVGRenderer` behavior and public surface while improving testability and
  extensibility.

## Scope
- Add a new `RendererInterface` in `donner/svg/renderer/` exposing frame control, state stack
  management, paint setup, and primitive drawing operations.
- Split the current `RendererSkia` into a backend-agnostic `RendererDriver` (scene traversal)
  and a Skia implementation of `RendererInterface`.
- Update render entry points (e.g., `SVGRenderer`) to use `RendererDriver` with a concrete
  `RendererInterface` instance.
- Add unit tests validating driver-to-interface interactions and smoke tests for Skia rendering.

## Non-Goals
- Changing SVG parsing, layout, or styling stages.
- Altering public APIs for document setup or resource loading.
- Rewriting existing rendering primitives beyond adapting them to the interface.

## Proposed Architecture
### RendererInterface
Located at `donner/svg/renderer/RendererInterface.h`. The interface must be Skia-free and lean on
Donner primitives already present in the rendering pipeline:
- Frame control: `beginFrame`/`endFrame` driven by a lightweight `RenderViewport` POD describing
  viewport size and device scale (no Skia surfaces are exposed).
- State stack: `pushTransform(const Transformd&)`/`popTransform()` and
  `pushClip(const ResolvedClip&)`/`popClip()` where `ResolvedClip` is a backend-neutral clip path
  derived from `ResolvedClipPath`/`ResolvedMask` data.
- Paint configuration: `setPaint(const PaintParams&)`, where `PaintParams` wraps resolved fill,
  stroke, filter, opacity, and stroke width data from `RenderingInstanceComponent`.
- Primitive drawing: `drawPath(const PathShape&, const StrokeParams&)`,
  `drawImage(const ImageResource&, const ImageParams&)`, and
  `drawText(const components::ComputedTextComponent&, const TextParams&)`, with `PathShape`
  wrapping a `PathSpline` plus fill rule. Simple shapes (`drawRect`, `drawEllipse`) are helpers
  over `PathShape`.
- Snapshotting: `takeSnapshot()` returns a backend-neutral bitmap wrapper (e.g., `RendererBitmap`)
  to preserve current `SVGRenderer` expectations without leaking Skia types.
- Lifetime: interface is non-owning; the driver controls when frames begin/end and when snapshots
  are requested.
- Documentation: the interface and all neutral structs include comprehensive Doxygen comments so
  downstream implementers have precise guidance on ownership and coordinate expectations.

#### Interface sketch
```cpp
class RendererInterface {
 public:
  virtual ~RendererInterface() = default;

  virtual void beginFrame(const RenderViewport& viewport) = 0;
  virtual void endFrame() = 0;

  virtual void pushTransform(const Transformd& transform) = 0;
  virtual void popTransform() = 0;

  virtual void pushClip(const ResolvedClip& clip) = 0;
  virtual void popClip() = 0;

  virtual void setPaint(const PaintParams& paint) = 0;

  virtual void drawPath(const PathShape& path, const StrokeParams& stroke) = 0;
  virtual void drawRect(const Boxd& rect, const StrokeParams& stroke) = 0;
  virtual void drawEllipse(const Boxd& bounds, const StrokeParams& stroke) = 0;
  virtual void drawImage(const ImageResource& image, const ImageParams& params) = 0;
  virtual void drawText(const components::ComputedTextComponent& text,
                        const TextParams& params) = 0;

  virtual RendererBitmap takeSnapshot() const = 0;
};
```
`RenderViewport`, `ResolvedClip`, `PaintParams`, `PathShape`, `StrokeParams`, `ImageParams`,
`TextParams`, and `RendererBitmap` are POD/struct wrappers over existing Donner math primitives
(`Boxd`, `Transformd`, `Vector2d`, `Lengthd`) and resolved style data. Implementations translate
these neutral structs to backend specifics (e.g., Skia paths, paints, and canvases) without
changing the interface surface.

### RendererDriver
- Responsible for traversing `SVGDocument` instances and invoking `RendererInterface` methods in
  render order.
- Extracts render instances and paint/style state, handling transforms, clips, masks, and opacity
  stacking without backend knowledge.
- Handles caching or batching decisions that are backend-independent; backend-specific caches
  remain within implementations.
- Surface management (viewport sizing, pixel ratio) is handled via driver inputs and forwarded to
  the interface.

### RendererSkia (implementation)
- Implements `RendererInterface` using Skia primitives and existing code paths.
- Owns Skia-specific resources (canvas, surface, font manager) and translates interface calls to
  Skia operations.
- Provides `bitmap()`/`takeSnapshot()` by returning the Skia bitmap or image currently used by
  render entry points.

### Construction and Wiring
- `SVGRenderer` constructs a `RendererSkia` and wraps it in a `RendererDriver`. Public
  `draw`/`bitmap` signatures remain unchanged; methods delegate to the driver, which uses the
  Skia implementation internally.
- Future backends (e.g., testing mocks or CPU-only rasterizers) can implement `RendererInterface`
  and be injected into the driver.

## Testing Strategy
- **Driver interaction tests:** Create a mock `RendererInterface` that records calls; verify
  traversal of representative SVG documents triggers expected sequences (transforms, clips, paint
  setup, primitives). Use focused fixtures covering groups, opacity, clipping, and text/images.
- **Skia smoke tests:** Use small SVG samples to render via `RendererDriver + RendererSkia`; compare
  key properties (non-empty bitmap, optional pixel hashes) to guard against regressions.
- **Snapshot compatibility:** Ensure `bitmap()`/`takeSnapshot()` behavior matches existing usage in
  `SVGRenderer` tests.

## Migration Plan
1. Introduce `RendererInterface` header and update includes.
2. Extract traversal code from `RendererSkia` into new `RendererDriver`; adjust build targets
   (Bazel/CMake) accordingly.
3. Adapt `RendererSkia` to implement the interface, keeping Skia-specific members and rendering
   logic.
4. Update `SVGRenderer` and related entry points to compose a driver with a concrete renderer.
5. Add interaction tests with a mock interface and run existing Skia rendering tests/smoke tests.
6. Clean up any remaining direct `RendererSkia` references; document new extension points.

## Risks and Mitigations
- **Behavioral drift:** Extensive traversal tests and Skia smoke tests reduce regression risk.
- **Performance:** The driver/interface boundary introduces minimal overhead; profiling can
  confirm no significant impact. Inline helpers and move-only data where beneficial.
- **API stability:** Retain existing public renderer APIs; internal refactor only.

## Open Questions
- Do we need additional primitives (e.g., gradients or filters) exposed directly on the interface
  for performance, or are they derived from `PaintParams`?
- Should `takeSnapshot()` return a backend-neutral image abstraction instead of `SkBitmap` for
  future backends?
- How should resource lifetimes (fonts, images) be managed across backends—via shared caches in
  the driver or per-backend ownership?

## Implementation Plan and TODOs
Every implementation milestone must include writing or updating tests to cover the new behavior,
using GoogleMock helpers and shared matchers to target high coverage as features land.
- **Interface definition**
  - [x] Add `donner/svg/renderer/RendererInterface.h` with frame control, state stack, paint,
    primitive APIs, and snapshot support.
  - [x] Ensure public comments clarify ownership semantics and snapshot expectations for
    `SVGRenderer`.
- **Driver extraction**
  - [ ] Introduce `RendererDriver` that traverses `SVGDocument` and emits interface calls,
    initially duplicating existing traversal logic from `RendererSkia`.
    - [x] Emit transforms, clips, paint state, and path primitives through `RendererInterface` as a
      first slice of traversal behavior.
    - [x] Route text and image primitives through the interface with neutral parameter shims.
  - [x] Initialize the render tree and frame lifecycle in `RendererDriver`, feeding the neutral
    interface for subsequent traversal work.
  - [x] Add traversal scaffolding to iterate rendering instances so command emission can be
    layered in next steps.
  - [x] Update build targets (Bazel/CMake) to expose the driver as a separate library.
- **Skia backend adaptation**
  - [x] Refactor `RendererSkia` to implement `RendererInterface`, retaining Skia-specific
    resource management and translating interface calls to Skia primitives.
  - [x] Wire `bitmap()`/`takeSnapshot()` to existing Skia surface behavior for compatibility.
- **Renderer wiring**
  - [x] Route primary entry points (tooling, examples, viewer, and tests) through
    `RendererDriver` paired with a `RendererSkia` instance while keeping public
    rendering and snapshot behavior unchanged.
  - [ ] Remove remaining direct `RendererSkia` references outside the backend implementation.
- **Testing**
  - [x] Add mock-based interaction tests validating driver → interface call sequences for
    groups, transforms, clips, opacity, text, and images.
    - [x] Grow reusable GoogleMock fixtures/matchers for renderer calls so new traversal slices add
      coverage immediately.
  - [ ] Add Skia smoke/regression tests ensuring rendered bitmaps remain non-empty and snapshots
    align with pre-refactor expectations.
  - [ ] Run existing rendering tests to ensure no behavioral regressions.
  - **Resvg suite parity**
    - [ ] Port the resvg test suite to the driver-backed renderer path and capture any parity gaps.
    - [ ] Document and resolve discrepancies (thresholds, skips, or fixes) discovered during the
      migration.
    - [ ] Fix remaining stroke/dash/opacity regressions after enabling unit-aware stroke length
      resolution (mm/%/relative) in the driver and re-running the stroke-focused resvg shard.
    - [x] Start the migration by consuming driver snapshots in the image-comparison harness to
      exercise the backend-neutral API surface.
