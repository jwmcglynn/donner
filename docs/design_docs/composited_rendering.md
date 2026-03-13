# Design: Composited Rendering

**Status:** Design
**Author:** Claude Opus 4.6
**Created:** 2026-03-12
**Tracking:** v0.5 milestone ([ProjectRoadmap](ProjectRoadmap.md))

## Summary

Implement a layer-based composited rendering system that caches static portions of the
SVG render tree as pixmaps and only re-renders layers that have changed. Animated elements,
selected elements (in an editor), or any other frequently-changing subtree gets promoted to
its own compositing layer. The final image is assembled by compositing these content-sized
layer quads in paint order.

This is a general-purpose optimization that benefits:
- **Animation**: Only re-render the animated element's layer each frame; the static
  background and foreground layers are cached.
- **Interactive editing**: A selected/dragged element lives on its own layer, enabling
  real-time manipulation without re-rasterizing the rest of the document.
- **Incremental updates**: Any property change (style, geometry, text) only invalidates
  the layer(s) containing the affected element(s).

## Goals

- Cache static document content as pixmaps to avoid redundant rasterization.
- Re-render only the layers containing elements whose properties changed.
- Keep layers content-sized (tight bounding box), not full-image-sized.
- Compose layers as textured quads in paint order to produce the final image.
- Support both rendering backends (Skia, tiny-skia) through the existing
  `RendererInterface` abstraction.
- Make layer promotion pluggable: animation system, editor selection, or explicit hints
  can all trigger promotion.
- Maintain pixel-perfect correctness: composited output must match single-pass rendering.

## Non-Goals

- GPU-accelerated compositing (texture upload, GPU quad drawing). The compositor operates
  on CPU pixmaps. GPU acceleration can layer on top later.
- Automatic layer merging / de-promotion heuristics (e.g., browser-style "too many layers"
  consolidation). Start with explicit promotion only.
- Partial layer re-rasterization (dirty rectangles within a layer). Each dirty layer is
  fully re-rendered. Sub-layer invalidation is a future optimization.
- 3D transforms or perspective compositing.

## Background

### Current Single-Pass Architecture

Today, `RendererDriver` performs a single traversal of the flat render tree
(`RenderingInstanceView`), issuing drawing commands for every element via
`RendererInterface`. The backend rasterizes each command into a single output surface
(plus intermediate surfaces for opacity groups, filters, masks, and patterns).

For static SVGs, this is efficient. But when a small number of elements change
frequently (animation, editing), re-rasterizing the entire document each frame wastes
work. A 1000-element SVG with one spinning gear re-renders all 1000 elements per frame.

### Browser Compositing Precedent

All major browsers use compositing layers for performant animation:
- Chromium's `cc` layer tree promotes elements with animations/transforms to
  compositing layers, rasterizes each layer independently, and composites on the GPU.
- WebKit's `RenderLayer` tree with compositing decisions based on overlap, transforms,
  and animation.
- The key insight: **separate rasterization from composition**. Rasterize once, compose
  many times.

Donner's SVG-specific context simplifies several aspects:
- Strict document-order paint sequence (no z-index reordering).
- Flat render tree (already linearized by `RenderingContext`).
- No scrolling or viewport-relative positioning.
- Known set of animatable properties from the SMIL timing model.

## Design

### Layer Model

A **compositing layer** is a content-sized RGBA pixmap plus metadata:

```cpp
struct CompositingLayer {
  /// Unique ID for this layer.
  uint32_t id;

  /// Bounding box in document coordinates (defines pixmap position and size).
  Boxd bounds;

  /// Cached pixmap (rasterized content). Null if dirty.
  std::optional<tiny_skia::Pixmap> pixmap;

  /// Entity range: [firstEntity, lastEntity] in draw order.
  /// All entities in this range render into this layer.
  Entity firstEntity;
  Entity lastEntity;

  /// True if this layer needs re-rasterization.
  bool dirty = true;

  /// Reason this layer exists.
  enum class Reason {
    Static,         // Background/foreground grouping of unchanged elements
    Animation,      // Contains an animated element
    Selection,      // Contains a user-selected element (editor)
    Explicit,       // Programmatic promotion via API
  };
  Reason reason = Reason::Static;
};
```

### Layer Decomposition

The flat render tree is sliced into contiguous runs of entities. Each run becomes a layer.

Given entities in paint order `[E0, E1, E2, E3, E4, E5]` where `E2` and `E4` are
animated:

```
Paint order:  E0   E1   E2   E3   E4   E5
              ├────────┤ ├──┤ ├──┤ ├──┤ ├──┤
Layer:        Static-0   Anim  S-1  Anim S-2
              (cached)   (dyn) (c)  (dyn)(c)
```

Layer decomposition runs in `O(n)` over the entity list:
1. Walk entities in draw order.
2. If an entity (or its subtree) is promoted, start a new dynamic layer.
3. Consecutive non-promoted entities are grouped into a single static layer.

```
algorithm BuildLayers(entities, promotedSet):
    layers = []
    currentStaticStart = null

    for entity in entities (by drawOrder):
        if entity in promotedSet or entity is descendant of promoted subtree:
            // Flush pending static layer
            if currentStaticStart != null:
                layers.append(StaticLayer(currentStaticStart, prevEntity))
                currentStaticStart = null
            // Dynamic layer for this entity's subtree
            layers.append(DynamicLayer(entity, entity.lastDescendant))
            skip to entity.lastDescendant + 1
        else:
            if currentStaticStart == null:
                currentStaticStart = entity

    // Flush trailing static layer
    if currentStaticStart != null:
        layers.append(StaticLayer(currentStaticStart, lastEntity))

    return layers
```

### Subtree Containment and Compositing Contexts

An animated element and its **entire subtree** go on the same dynamic layer. This is
essential for correctness because:
- Child elements inherit the animated transform/style.
- Opacity groups, filters, masks applied to the animated element must composite
  the full subtree before blending into the final image.

**Compositing context escalation**: If an ancestor of an animated element has
`opacity < 1`, a filter, a mask, or a clip-path, the ancestor's entire subtree must
be on the same layer. This ensures the compositing effect (e.g., group opacity) is
applied correctly.

```
<g opacity="0.5">          <!-- Compositing context -->
  <rect/>                  <!-- Must be on same layer as animated child -->
  <circle animate="..."/>  <!-- Animated -->
  <rect/>                  <!-- Must be on same layer too -->
</g>
```

In this case, the entire `<g>` subtree is promoted to a dynamic layer.

The escalation algorithm:
1. Mark animated entities as promoted.
2. Walk up each promoted entity's ancestor chain.
3. If an ancestor has opacity/filter/mask/clip-path, mark the ancestor's subtree
   as promoted instead.
4. Merge overlapping promoted subtrees.

### Layer Bounds

Each layer's bounds are the union of the visual bounding boxes of all entities in the
layer, expanded to include:
- Stroke width (half stroke width on each side).
- Filter effects region (filters can expand beyond geometry bounds).
- Marker extents.

Bounds are computed in document coordinates and rounded to integer pixels at the target
device pixel ratio. The pixmap is allocated at this integer size.

Layers are **content-sized**: a small animated spinner in the corner of a large SVG
produces a small dynamic layer pixmap, not a full-document-sized one.

### Rendering Pipeline

```
  ┌─────────────────────────────────────────────────────────────┐
  │ 1. Layer Assignment (once, or when tree structure changes)  │
  │    - Identify promoted entities (animated, selected, etc.)  │
  │    - Build layer list from flat render tree                 │
  │    - Compute layer bounds                                   │
  └──────────────────────┬──────────────────────────────────────┘
                         │
  ┌──────────────────────▼──────────────────────────────────────┐
  │ 2. Layer Invalidation (each frame)                          │
  │    - Animation system updates animated values               │
  │    - Mark dynamic layers as dirty                           │
  │    - Static layers remain cached unless tree changed        │
  └──────────────────────┬──────────────────────────────────────┘
                         │
  ┌──────────────────────▼──────────────────────────────────────┐
  │ 3. Layer Rasterization (each frame, only dirty layers)      │
  │    - For each dirty layer:                                  │
  │      - Create/resize pixmap to layer bounds                 │
  │      - Set up renderer with layer-local transform           │
  │      - Render entity range [first..last] into layer pixmap  │
  │      - Mark layer as clean                                  │
  └──────────────────────┬──────────────────────────────────────┘
                         │
  ┌──────────────────────▼──────────────────────────────────────┐
  │ 4. Layer Composition (each frame)                           │
  │    - Clear output surface                                   │
  │    - For each layer in paint order:                         │
  │      - Draw layer pixmap at layer.bounds position           │
  │    - Output final composited image                          │
  └─────────────────────────────────────────────────────────────┘
```

### Rasterization: Per-Layer Rendering

Each layer is rendered by driving a subset of the flat render tree through
`RendererDriver`. The key change is a **layer-local coordinate system**:

```cpp
void rasterizeLayer(CompositingLayer& layer, RendererInterface& renderer) {
  // Layer pixmap origin = layer.bounds.topLeft()
  // Translate so layer content starts at (0,0) in the pixmap.
  Transformd layerTransform = Transformd::Translate(
      -layer.bounds.x(), -layer.bounds.y());

  renderer.beginFrame(RenderViewport{layer.bounds.size(), devicePixelRatio});

  // Render only entities in this layer's range.
  // RendererDriver already supports range rendering via the flat entity list.
  driver.renderEntityRange(layer.firstEntity, layer.lastEntity, layerTransform);

  renderer.endFrame();
  layer.pixmap = renderer.takeSnapshot();
  layer.dirty = false;
}
```

The existing `RendererDriver` already stores a `layerBaseTransform_` that is composed
with entity transforms. This maps directly to the layer-local offset.

### Composition

Composition is a simple loop over cached layer pixmaps:

```cpp
void composeLayers(const std::vector<CompositingLayer>& layers,
                   RendererInterface& output) {
  output.beginFrame(viewport);
  for (const auto& layer : layers) {
    // Draw layer.pixmap as an image at layer.bounds position.
    output.drawImage(layer.pixmap, ImageParams{layer.bounds});
  }
  output.endFrame();
}
```

This is intentionally simple. Each layer is a pre-rasterized quad placed at its
document-space position. The compositor does no clipping, no transform math, no style
resolution — all of that was handled during per-layer rasterization.

For layers with `opacity < 1` applied at the layer level (not per-element), the
compositor applies opacity during `drawImage`.

### Layer Promotion API

Layer promotion is driven by a pluggable `LayerPromotionPolicy`:

```cpp
class LayerPromotionPolicy {
public:
  virtual ~LayerPromotionPolicy() = default;

  /// Return the set of entities that should be promoted to their own layers.
  /// Called during layer assignment.
  virtual std::vector<Entity> getPromotedEntities(
      const Registry& registry) const = 0;
};
```

Built-in policies:

```cpp
/// Promotes entities that have active animations.
class AnimationLayerPolicy : public LayerPromotionPolicy {
  std::vector<Entity> getPromotedEntities(const Registry& registry) const override {
    std::vector<Entity> result;
    for (auto [entity, state] :
         registry.view<AnimationStateComponent>().each()) {
      if (state.phase == AnimationPhase::Active ||
          state.phase == AnimationPhase::Frozen) {
        result.push_back(state.targetEntity);
      }
    }
    return result;
  }
};

/// Promotes explicitly selected entities (for editor use).
class SelectionLayerPolicy : public LayerPromotionPolicy {
  std::vector<Entity> promoted_;
public:
  void select(Entity e) { promoted_.push_back(e); }
  void deselect(Entity e) { std::erase(promoted_, e); }
  std::vector<Entity> getPromotedEntities(const Registry&) const override {
    return promoted_;
  }
};

/// Combines multiple policies.
class CompositeLayerPolicy : public LayerPromotionPolicy {
  std::vector<std::unique_ptr<LayerPromotionPolicy>> policies_;
public:
  void add(std::unique_ptr<LayerPromotionPolicy> p) {
    policies_.push_back(std::move(p));
  }
  std::vector<Entity> getPromotedEntities(const Registry& registry) const override {
    std::vector<Entity> result;
    for (const auto& p : policies_) {
      auto entities = p->getPromotedEntities(registry);
      result.insert(result.end(), entities.begin(), entities.end());
    }
    // Deduplicate
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
  }
};
```

### Invalidation Strategy

**When does a layer become dirty?**

| Event | Effect |
|-------|--------|
| Animation tick changes a value | Dynamic layer containing target entity marked dirty |
| Element style/attribute edited | Layer containing edited entity marked dirty |
| Element added/removed | Full layer reassignment (structural change) |
| Viewport resize | All layers invalidated (pixmap sizes change) |
| Selection change (editor) | Layer reassignment (promotion set changed) |

Animation-driven invalidation is the common case. The `AnimationSystem::advance()`
call already knows which entities have active animations and which values changed.
It can directly mark the containing layer dirty:

```cpp
void AnimationSystem::advance(double documentTime) {
  // ... existing animation evaluation ...

  // After computing new values, mark layers dirty:
  for (auto [entity, state] : registry.view<AnimationStateComponent>().each()) {
    if (state.phase == AnimationPhase::Active) {
      if (auto* layerRef = registry.try_get<LayerMembershipComponent>(entity)) {
        layerRef->layer->dirty = true;
      }
    }
  }
}
```

### Layer Bounds Updates

Dynamic layer bounds may change as animations progress (e.g., an element moving
across the canvas). Two strategies:

1. **Conservative bounds**: Compute bounds from the animation's full value range
   (e.g., motion path bounding box). Layer pixmap is allocated once and never resized.
   Wastes some memory but avoids per-frame reallocation.

2. **Adaptive bounds**: Recompute bounds each frame from the current animated value.
   Reallocate pixmap only when bounds grow. Handles unbounded animations (e.g.,
   `repeatCount="indefinite"` with `by` accumulation) but adds per-frame overhead.

Recommendation: Start with **conservative bounds** for animations (the timing model
provides enough information to compute the full range) and **adaptive bounds** for
editor interactions (drag extent is unknown).

### Memory Budget

Each layer consumes `width * height * 4` bytes (RGBA). For a 1920x1080 SVG:
- Full-size layer: ~8 MB
- 200x200 animated spinner layer: ~160 KB
- 10 layers total: typically 10-20 MB

A memory budget can cap the total layer pixmap memory. If exceeded, merge the
smallest adjacent static layers until under budget.

### Integration with Existing Architecture

The composited rendering system is an **optional optimization layer** that wraps the
existing `RendererDriver` + `RendererInterface` pipeline. It does not replace them.

```
                    ┌─────────────────────────┐
                    │    CompositedRenderer    │
                    │  (layer management +     │
                    │   composition)           │
                    └────────┬────────────────┘
                             │ delegates to
                    ┌────────▼────────────────┐
                    │   RendererDriver         │
                    │  (entity traversal +     │
                    │   drawing commands)       │
                    └────────┬────────────────┘
                             │ calls
                    ┌────────▼────────────────┐
                    │   RendererInterface      │
                    │  (Skia / tiny-skia)      │
                    └─────────────────────────┘
```

**New files:**

| File | Purpose |
|------|---------|
| `donner/svg/renderer/CompositedRenderer.h` | Layer management, composition loop |
| `donner/svg/renderer/CompositedRenderer.cc` | Implementation |
| `donner/svg/renderer/LayerPromotionPolicy.h` | Policy interface + built-in policies |
| `donner/svg/components/LayerMembershipComponent.h` | Entity-to-layer reverse mapping |

**Changes to existing files:**

| File | Change |
|------|--------|
| `RendererDriver.h/.cc` | Add `renderEntityRange()` for rendering a subset of the flat tree |
| `RenderingContext.cc` | Optional: pass `CompositedRenderer` instead of direct `RendererDriver` |
| `AnimationSystem.cc` | Mark dirty layers after animation evaluation |
| `Document.h` | Add `enableCompositedRendering()` API |

### Public API

```cpp
class Document {
public:
  /// Enable composited rendering with the given promotion policy.
  /// When enabled, renderToPixmap() uses layer caching.
  void enableCompositedRendering(
      std::unique_ptr<LayerPromotionPolicy> policy = nullptr);

  /// Existing API — now uses composited rendering if enabled.
  RendererBitmap renderToPixmap(const RenderViewport& viewport);

  /// Advance animation and re-render only dirty layers.
  /// Returns the composited result.
  RendererBitmap renderFrame(double documentTime);
};
```

For the editor use case:

```cpp
// Editor creates a selection policy and enables compositing.
auto selectionPolicy = std::make_shared<SelectionLayerPolicy>();
auto compositePolicy = std::make_unique<CompositeLayerPolicy>();
compositePolicy->add(std::make_unique<AnimationLayerPolicy>());
compositePolicy->add(selectionPolicy);  // shared_ptr for editor access
document.enableCompositedRendering(std::move(compositePolicy));

// User selects an element — promote to own layer.
selectionPolicy->select(selectedEntity);

// User drags element — only the selection layer re-renders.
auto* transform = registry.try_get<TransformComponent>(selectedEntity);
transform->transform.set(newTransform, css::Specificity::Override());
document.renderFrame(currentTime);  // Fast: only dirty layers re-render.
```

## Implementation Plan

### Phase 1: Layer Infrastructure ✅

- [x] Add `CompositingLayer` struct and `LayerMembershipComponent`
- [x] Implement layer decomposition algorithm (flat tree → layer list)
- [x] Implement compositing context escalation (ancestor opacity/filter/mask)
- [x] Implement layer bounds computation (visual bounding box union) — implemented in Phase 6
- [x] Tests: 14 tests covering no-promotion, single/multiple promotion, adjacent,
      subtree, compositing context escalation (opacity, clip-path, nested), membership
      assignment, selection reason, edge cases

### Phase 2: Per-Layer Rasterization ✅

- [x] Add `RendererDriver::drawEntityRange()` for subset rendering
- [x] Implement `CompositedRenderer::rasterizeLayer()` with offscreen instance
- [x] Implement `CompositedRenderer::composeLayers()` via `drawImage()`
- [x] Verify pixel-perfect match: composited output == single-pass output
- [x] Tests: 8 tests covering single-pass matching, dirty tracking, opacity groups,
      promoted entities, and layer count verification

### Phase 3: Invalidation and Caching ✅

- [x] `markEntityDirty(Entity)` — maps data or render entity to layer via
      `LayerMembershipComponent` and marks it dirty
- [x] `invalidateAnimatedLayers()` — scans `AnimatedValuesComponent` and marks
      affected layers dirty
- [x] `FrameStats` tracking — counts layers rasterized vs reused per frame
- [x] Frame rendering loop: invalidate → rasterize dirty → compose (verified
      that clean layers are skipped)
- [x] Tests: 8 new tests covering frame stats, entity dirty marking (data and
      render entity paths), unknown entity handling, animation invalidation
- [ ] Conservative bounds for animation layers (deferred to Phase 6)
- [ ] Performance benchmarks (deferred to Phase 6)

### Phase 4: Editor Integration ✅

- [x] `SelectionLayerPolicy` — concrete `LayerPromotionPolicy` for editor
      selection (add/remove/clear/set entities)
- [x] `compositionTransform` on `CompositingLayer` — per-layer transform
      applied at composition time, no re-rasterization
- [x] `setLayerTransform()` / `setEntityLayerTransform()` — transform-only
      composition API
- [x] `findLayerForEntity()` — entity→layer lookup (data or render entity)
- [x] Tests: 5 new tests covering SelectionLayerPolicy, entity-to-layer lookup,
      transform-only translation, transform reset, layer transform by ID
- [ ] Adaptive bounds for selection layers (deferred to Phase 6)
- [ ] `Document::enableCompositedRendering()` public API (deferred — needs
      concrete consumer)
- [ ] Memory budget and layer merging (deferred to Phase 6)

### Phase 5: Async Rendering and Transform-Only Composition ✅

- [x] Transform-only composition: `compositionTransform` + `setLayerTransform()`
      (implemented in Phase 4)
- [x] `composeOnly()` — fast composition path that skips rasterization entirely,
      uses stale cached pixmaps at their current composition transforms
- [x] `renderPredicted()` / `swapPredicted()` — pre-render dirty layers into
      a separate buffer without clobbering the active cache; swap when ready
- [x] Stale layer reuse: dirty layers retain their previous pixmaps during
      `composeOnly()` for immediate visual feedback
- [x] Tests: 4 new tests covering composeOnly, composeOnly with transforms,
      renderPredicted + swap lifecycle, selective predicted rendering
- [ ] Background thread rasterization (std::async) — deferred; the API supports
      it but the threading wrapper is not yet implemented

### Phase 6: Optimizations ✅

- [x] Content-sized layers: layer pixmaps sized to visual bounding box of entities,
      not full canvas. Bounds computed from `ComputedPathComponent::spline.bounds()`
      transformed by `entityFromWorldTransform`, inflated by stroke width, clamped to
      canvas, and rounded to integer pixels. Layers with no geometry fall back to full
      canvas bounds.
- [x] Tests: `LayerBoundsAreContentSized` verifies dynamic layer bounds are tight
      and composited output matches single-pass rendering pixel-for-pixel.
- [x] Layer pixmap pooling: offscreen renderer instances cached per layer and
      reused across frames; `ImageResource` cached per layer and only rebuilt
      after rasterization (not on every composition frame). `FrameStats` tracks
      `offscreenPoolHits` and `imagePoolHits`. 3 new tests verify pool reuse,
      image cache hits, and pixel-perfect correctness after pooling.
- [x] Incremental layer reassignment: when `prepare()` is called again with the
      same promotion set and canvas size, skips decomposition and preserves
      cached pixmaps. Only recomputes bounds and marks layers dirty if bounds
      changed. Full rebuild triggered only when promotion set, reason, or canvas
      size changes. 3 new tests verify cache preservation, pixel-perfect
      correctness, and full rebuild on set change.
- [x] Opacity layer optimization: `compositionOpacity` on `CompositingLayer` applied
      via `ImageParams::opacity` during composition. `setLayerOpacity()` and
      `setEntityLayerOpacity()` API methods allow opacity changes without
      re-rasterization. 3 new tests verify visible effect, zero re-rasterization,
      and layer ID access.
- [x] Composition caching: `compositionDirty_` flag tracks whether recomposition
      is needed. `renderFrame()` skips `composeLayers()` entirely when no layers
      were rasterized and no composition parameters changed (renderer surface
      preserves the previous composited output). `setLayerTransform()`,
      `setLayerOpacity()`, and `prepare()` (full rebuild) set the dirty flag.
      True damage-rect-based partial composition deferred (requires renderer-level
      incremental update support).

## Async Rendering for 60 FPS Editing

### Problem

Even with composited rendering, re-rasterizing a complex layer (e.g., a group with
many child paths, filters, or text) can take >16ms, causing dropped frames during
interactive editing. For a smooth move/rotate/scale experience, composition must run
at 60 FPS regardless of rasterization cost.

### Solution: Stale Layer Reuse with Async Re-render

When a layer is marked dirty, the compositor does **not** block on re-rasterization.
Instead:

1. **Immediate composition**: Use the layer's previous cached pixmap, applying the
   new transform at the composition stage. For a pure translation, this means drawing
   the old pixmap at the new position — no quality loss. For rotation/scale, the old
   pixmap is composited with bilinear sampling, producing slight blur at the new
   orientation — acceptable for interactive feedback.

2. **Background re-render**: Kick off the layer rasterization on a background thread.
   The rasterizer renders into a new pixmap (double-buffered).

3. **Hot-swap**: When the background render completes, atomically swap the new pixmap
   into the layer. The next composition frame picks up the crisp, correctly-rendered
   content.

```
Frame N:   [Compose with stale layer + new transform] → 60 FPS output
Frame N+1: [Compose with stale layer + new transform] → 60 FPS output
Frame N+2: [Background render completes, swap pixmap]
Frame N+3: [Compose with fresh layer] → crisp output
```

### Transform-Only Composition (No Re-render)

Many interactive operations only change an element's transform (translate, rotate,
scale). These should **never** trigger a layer re-render:

| Operation | Composition | Re-render? | Quality |
|-----------|-------------|------------|---------|
| Translate (drag/move) | Reposition quad | No | Pixel-perfect |
| Rotate | Rotate quad (bilinear) | Async | Stale until complete |
| Scale | Scale quad (bilinear) | Async | Stale until complete |
| Style change (fill, opacity) | Use stale pixmap | Async | Stale until complete |
| Geometry change (path d) | Use stale pixmap | Async | Stale until complete |

Pure translation is pixel-perfect with zero re-render cost. Rotation and scale use
the stale pixmap with bilinear resampling for immediate feedback, then kick off an
async re-render. When the re-render completes, the crisp pixmap replaces the stale one.

The `CompositingLayer` tracks a **composition transform** separate from the
rasterized content:

```cpp
struct CompositingLayer {
  // ... existing fields ...

  /// Transform applied at composition time (relative to layer.bounds origin).
  /// Updated immediately on interactive manipulation; does NOT trigger re-render.
  Transformd compositionTransform;

  /// The rasterized content's transform at the time of last render.
  /// When compositionTransform diverges from this, the layer shows
  /// stale-but-transformed content until the async re-render completes.
  Transformd rasterizedTransform;
};
```

Composition draws each layer as:
```cpp
drawImage(layer.pixmap,
          layer.bounds,
          layer.compositionTransform * inverse(layer.rasterizedTransform));
```

For pure translation, `compositionTransform` and `rasterizedTransform` differ only
in translation — the pixmap content is pixel-perfect at the new position. No blur,
no quality loss, no re-render.

### Predictive Re-rendering for Animations

Animations are deterministic — the timing model knows exactly when each animation
is active and what values it will produce at future times. This enables **predictive
re-rendering**: while the current frame is being displayed, the render thread can
start rasterizing the next frame's layer content.

```
Time T:     Display frame T, start rendering frame T+1 in background
Time T+Δt:  Display frame T+1 (already rendered), start rendering frame T+2
```

For smooth animations, the compositor maintains a 1-frame render-ahead buffer per
dynamic layer. The animation system provides the next frame's document time to the
predictive renderer, which evaluates animated values at that time and rasterizes.

Start with a fixed 1-frame render-ahead buffer. In the future, the buffer depth
could be dynamic — for repeating animations or predictable keyframe sequences, the
renderer could pre-render multiple frames ahead when background thread capacity
allows. But 1-frame-ahead is the scope for now.

This is particularly effective for:
- **Continuous animations** (`repeatCount="indefinite"`): the value at `t + dt` is
  trivially computable from the timing model.
- **Keyframe animations**: the next keyframe value and interpolation progress are
  known in advance.
- **Motion paths**: the next position along the path is computable from arc-length
  parameterization.

The predictive render can be speculative — if user interaction changes the animation
state (e.g., pausing), the pre-rendered frame is discarded and a fresh render is
triggered. The cost of a discarded frame is just wasted background thread time; the
main thread (composition) is never blocked.

### Thread Safety

- **Main thread**: Runs composition, handles user input, updates `compositionTransform`.
- **Render thread**: Rasterizes dirty layers into new pixmaps.
- **Synchronization**: Double-buffered pixmaps per layer. The render thread writes to
  the back buffer; on completion, an atomic swap makes it the front buffer. The main
  thread only reads the front buffer during composition.
- No locks on the hot path. The only synchronization point is the atomic pixmap pointer
  swap.

## Testing Requirements

### Fuzz Testing

Every parser added to the codebase must have a corresponding fuzz test. This is a
hard requirement — no parser ships without a fuzzer.

Existing parsers that need fuzz tests (backlog):
- `ClockValueParser` (animation timing values)
- `SyncbaseRef` parsing in `AttributeParser`
- `AnimateValue` parsing (from/to/by/values for `<animate>`)
- `AnimateTransformValue` parsing (type-specific value lists)
- `AnimateMotionPath` parsing (path data for `<animateMotion>`)

New parsers introduced by composited rendering (if any) must include fuzz tests
in the same PR.

## Correctness Considerations

### Paint Order Preservation

SVG mandates strict document-order painting. The layer system preserves this because:
1. Layers are contiguous entity ranges from the flat (paint-ordered) render tree.
2. Layers are composed in the same order they were extracted.
3. No entity appears in more than one layer.

### Opacity and Blending

An element with `opacity < 0.5` that overlaps elements in both the layer below and
above it must produce the same blending result. This is correct because:
- The element is fully rasterized into its own layer (including opacity application).
- The layer pixmap is composited over the layers below using source-over blending.
- This matches single-pass behavior where the element is rasterized with opacity
  into the output surface.

### Filter Effects

Filters that reference `BackgroundImage` or `BackgroundAlpha` standard inputs would
need cross-layer access. However, these inputs are deprecated in SVG2 and not
implemented in donner. All implemented filter standard inputs (`SourceGraphic`,
`SourceAlpha`, `FillPaint`, `StrokePaint`) are local to the filtered element's layer.

### Clip Paths and Masks

Clip paths and masks are applied during per-layer rasterization, not during
composition. Since an element and all its clipping/masking context are on the same
layer (guaranteed by compositing context escalation), correctness is maintained.

## Open Questions

1. **Layer granularity for multiple animations on the same element**: If both `fill`
   and `transform` are animated on the same element, that's one layer. But if two
   sibling elements are both animated, should they share a layer or get separate ones?
   Separate layers enable independent caching but increase memory and composition cost.
   Recommendation: separate layers by default; provide a `LayerGroup` hint for manual
   merging.

2. **Inter-layer effects**: If a future implementation adds `BackgroundImage` filter
   input support, the compositor would need to provide the composited-so-far result to
   filter layers. This is architecturally possible (compose layers 0..N-1, pass result
   as BackgroundImage to layer N) but adds complexity.

3. **Text layers**: Text elements with animated `textLength` or `startOffset` may
   change bounds significantly. Should text layers use adaptive bounds by default?
   Recommendation: yes, since text reshaping changes layout unpredictably.

4. **Composited rendering as default**: Should composited rendering be the default
   mode once stable, or always opt-in? For documents with no animations and no editor,
   the overhead of layer management + composition may exceed the single-pass cost.
   Recommendation: auto-enable when `Document::setTime()` is called or when an editor
   policy is registered; otherwise use single-pass.

## Performance Model

For a document with `N` elements, `A` animated elements, and `L` layers:

| Operation | Single-pass | Composited (steady state) |
|-----------|-------------|--------------------------|
| Rasterization | `O(N)` per frame | `O(A_elements)` per frame |
| Composition | N/A | `O(L)` blits per frame |
| Memory | 1 surface | `L` surfaces |
| Layer assignment | N/A | `O(N)` on structural change |

For a typical case (1000 elements, 3 animated, 7 layers):
- Single-pass: rasterize 1000 elements/frame.
- Composited: rasterize ~3 elements/frame + compose 7 layers.
- Speedup: ~100x rasterization reduction.

The composition cost (7 pixmap blits) is negligible compared to rasterizing 1000
elements with path tessellation, style resolution, and paint evaluation.

## References

- [Chromium Compositing](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/how_cc_works.md) — Browser compositor architecture
- [SVG Animations Level 2](https://svgwg.org/specs/animations/) — Animation timing model
- [CSS Compositing and Blending](https://www.w3.org/TR/compositing-1/) — Compositing semantics
- [Flutter Layer Tree](https://github.com/engine/docs/layers.md) — Similar quad-based compositing
