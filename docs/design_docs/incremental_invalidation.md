# Design: Incremental Invalidation

**Status:** Design
**Author:** Claude Opus 4.6
**Created:** 2026-03-13
**Tracking:** v0.5 milestone ([ProjectRoadmap](ProjectRoadmap.md))

## Summary

Replace Donner's current full-tree recomputation model with incremental invalidation: when a
DOM mutation (style change, attribute edit, tree insertion) occurs, only the affected elements
and their dependents are recomputed. The system tracks dirty state at five levels — style,
layout, shape, paint, and render instances — and propagates invalidation through the dependency
graph so that each `instantiateRenderTree()` call does minimal work.

## Goals

- DOM mutations that affect a single element should not trigger full-document recomputation.
- Style inheritance invalidation should cascade to descendants but not siblings or ancestors.
- Layout invalidation (transforms, viewBox, size) should cascade only to the affected subtree.
- The composited rendering layer system should receive fine-grained dirty notifications,
  allowing per-layer re-rasterization without full document re-render.
- Maintain pixel-perfect correctness: incremental output must match full recomputation output.

## Non-Goals

- GPU-accelerated dirty rectangle tracking (sub-layer partial re-rasterization).
- Concurrent/parallel style resolution across subtrees.
- CSS selector index (inverted index from property → matching elements). This is a future
  optimization for stylesheet-level changes.
- Animation-specific optimizations beyond what the composited renderer already provides.

## Background

### Current Architecture: Full-Tree Recomputation

Today, every call to `RenderingContext::instantiateRenderTree()` runs an 8-step pipeline
that recomputes **everything** from scratch:

```
instantiateRenderTree()
  └─→ createComputedComponents()
        1. Setup shadow trees (clipping, masking, patterns, markers)
        2. Evaluate and propagate ALL styles (StyleSystem::computeAllStyles)
        3. Instantiate shadow trees + propagate styles into them
        4. Determine ALL element sizes/layout (LayoutSystem)
        5. Compute ALL transforms
        6. Decompose ALL shapes to paths (ShapeSystem)
        7. Resolve ALL fill/stroke paint references (PaintSystem)
        8. Resolve ALL filter references (FilterSystem)
  └─→ instantiateRenderTreeWithPrecomputedTree()
        Creates RenderingInstanceComponent for every renderable element
```

For a 1000-element document where one element's `fill` attribute changes, this recomputes
all 1000 elements' styles, layouts, shapes, and paints. The actual changed element needs
~1ms of work; the full recomputation takes ~50ms.

### Existing Invalidation Mechanisms

The codebase already has per-system invalidation methods, but they're incomplete:

| Method | What it does | What it doesn't do |
|--------|-------------|-------------------|
| `StyleSystem::invalidateComputed(handle)` | Removes `ComputedStyleComponent` | Doesn't cascade to children that inherit |
| `StyleSystem::invalidateAll(handle)` | Removes style + marks for reparse | Doesn't cascade |
| `LayoutSystem::invalidate(handle)` | Clears cached viewBox/transforms | Doesn't cascade to descendants |
| `SVGGeometryElement::invalidate()` | Removes `ComputedPathComponent` | Doesn't invalidate paint/render |
| `RenderingContext::invalidateRenderTree()` | Clears ALL render instances | Nuclear option — no granularity |

The composited renderer (`CompositedRenderer`) already has fine-grained layer dirty tracking
via `markEntityDirty(Entity)`, `markLayerDirty(uint32_t)`, and `invalidateAnimatedLayers()`.
This design connects DOM mutations to that existing layer system.

### Dependency Graph

Understanding which computations depend on which inputs is critical:

```
DOM State (source of truth)
  │
  ├─→ StyleComponent (inline styles, class, style attribute)
  │     └─→ ComputedStyleComponent (cascade + inheritance)
  │           ├─→ ComputedLocalTransformComponent (transform property)
  │           ├─→ ComputedSizedElementComponent (x, y, width, height)
  │           ├─→ ComputedPathComponent (shape attributes via style)
  │           ├─→ ResolvedPaintServer (fill, stroke references)
  │           └─→ FilterEffect resolution
  │
  ├─→ TransformComponent (transform attribute)
  │     └─→ ComputedLocalTransformComponent
  │           └─→ ComputedAbsoluteTransformComponent (accumulates up tree)
  │                 └─→ RenderingInstanceComponent (world-space bounds)
  │
  ├─→ SizedElementComponent (x, y, width, height attributes)
  │     └─→ ComputedSizedElementComponent
  │           └─→ ComputedViewBoxComponent
  │
  ├─→ PathComponent (d, points, r, cx, etc.)
  │     └─→ ComputedPathComponent
  │
  └─→ Tree structure (parent/child relationships)
        └─→ Everything (shadow trees, inheritance, draw order)
```

## Design

### Dirty Flags Component

A single per-entity component tracks which aspects need recomputation:

```cpp
/// Tracks which computed properties are stale and need recomputation.
/// Attached to entities that have been mutated since last render.
struct DirtyFlagsComponent {
  enum Flags : uint16_t {
    None          = 0,
    Style         = 1 << 0,  // ComputedStyleComponent needs recomputation
    Layout        = 1 << 1,  // ComputedSizedElementComponent / viewBox
    Transform     = 1 << 2,  // ComputedLocalTransformComponent
    WorldTransform= 1 << 3,  // ComputedAbsoluteTransformComponent
    Shape         = 1 << 4,  // ComputedPathComponent
    Paint         = 1 << 5,  // ResolvedPaintServer (fill/stroke)
    Filter        = 1 << 6,  // Filter effect resolution
    RenderInstance= 1 << 7,  // RenderingInstanceComponent
    ShadowTree    = 1 << 8,  // Shadow tree needs re-instantiation

    // Compound flags for common patterns
    StyleCascade  = Style | Paint | Filter | RenderInstance,
    LayoutCascade = Layout | Transform | WorldTransform | RenderInstance,
    All           = 0xFFFF,
  };

  uint16_t flags = Flags::None;

  void mark(Flags f) { flags |= f; }
  bool test(Flags f) const { return (flags & f) != 0; }
  void clear(Flags f) { flags &= ~f; }
  void clearAll() { flags = Flags::None; }
};
```

Using a component (rather than a field on each computed component) keeps the data
compact and allows efficient ECS queries: `registry.view<DirtyFlagsComponent>()` gives
all entities that need work.

### Invalidation Propagation Rules

When a mutation occurs, dirty flags propagate according to these rules:

#### 1. Style Change (CSS property, `style` attribute, `class` attribute)

```
Element E gets Style dirty
  └─→ For each descendant D that inherits from E:
        If the changed property is inherited (color, font-*, fill, stroke, etc.):
          D gets Style dirty
        Else:
          Skip D (non-inherited properties don't cascade)
  └─→ E and affected descendants get Paint, Filter, RenderInstance dirty
```

**Optimization:** Track which properties changed. If only `opacity` changed (non-inherited,
no paint/filter impact), only mark `RenderInstance` dirty on E. If `color` changed
(inherited), cascade `Style` to all descendants.

#### 2. Transform Change (`transform` attribute, `transform-origin`)

```
Element E gets Transform dirty
  └─→ E gets WorldTransform dirty
  └─→ For each descendant D of E:
        D gets WorldTransform dirty
        D gets RenderInstance dirty
```

Transform changes cascade `WorldTransform` to all descendants because the absolute
transform is the product of all ancestor transforms.

#### 3. Layout Change (x, y, width, height, viewBox)

```
Element E gets Layout dirty
  └─→ If E defines a viewBox:
        All descendants get Layout dirty (viewport changed)
  └─→ E gets Transform, WorldTransform, RenderInstance dirty
  └─→ Descendants get WorldTransform, RenderInstance dirty
```

#### 4. Shape Change (path `d`, circle `r`, rect attributes)

```
Element E gets Shape dirty
  └─→ E gets RenderInstance dirty
  └─→ No cascade (shape is element-local)
```

#### 5. Tree Structure Change (appendChild, removeChild, insertBefore)

```
Full invalidation of affected subtrees:
  └─→ Removed subtree: remove all computed components
  └─→ Inserted subtree: mark All dirty on all entities in subtree
  └─→ Parent: mark ShadowTree dirty (draw order may change)
  └─→ Full render tree rebuild (draw order linearization)
```

Tree structure changes are the most expensive because they affect draw order, which
requires re-linearizing the render tree. This is inherently `O(n)` over the tree.

#### 6. Stylesheet Change (external stylesheet loaded, `<style>` element modified)

```
All entities get Style dirty (worst case)
  └─→ Future optimization: CSS selector index to narrow affected elements
```

### Selective Recomputation

`createComputedComponents()` is modified to skip clean entities:

```cpp
void RenderingContext::createComputedComponents(std::vector<ParseError>* outWarnings) {
  auto dirtyView = registry_.view<DirtyFlagsComponent>();

  if (dirtyView.empty() && !fullRebuildRequired_) {
    return;  // Nothing changed — skip entirely
  }

  if (fullRebuildRequired_) {
    // Tree structure changed — must do full recomputation
    // (same as today's code path)
    fullRecompute(outWarnings);
    fullRebuildRequired_ = false;
    return;
  }

  // Incremental path: only recompute dirty entities

  // 1. Shadow trees (only if ShadowTree dirty)
  for (auto [entity, dirty] : dirtyView.each()) {
    if (dirty.test(DirtyFlagsComponent::ShadowTree)) {
      recreateShadowTrees(EntityHandle(registry_, entity));
      dirty.clear(DirtyFlagsComponent::ShadowTree);
    }
  }

  // 2. Styles (only Style-dirty entities)
  std::vector<Entity> styleDirty;
  for (auto [entity, dirty] : dirtyView.each()) {
    if (dirty.test(DirtyFlagsComponent::Style)) {
      styleDirty.push_back(entity);
    }
  }
  if (!styleDirty.empty()) {
    StyleSystem().computeStylesFor(registry_, styleDirty, outWarnings);
    for (auto e : styleDirty) {
      dirtyView.get<DirtyFlagsComponent>(e).clear(DirtyFlagsComponent::Style);
    }
  }

  // 3. Layout (only Layout-dirty entities)
  for (auto [entity, dirty] : dirtyView.each()) {
    if (dirty.test(DirtyFlagsComponent::Layout)) {
      recomputeLayout(EntityHandle(registry_, entity), outWarnings);
      dirty.clear(DirtyFlagsComponent::Layout);
    }
  }

  // 4. Transforms (only Transform/WorldTransform-dirty entities)
  for (auto [entity, dirty] : dirtyView.each()) {
    if (dirty.test(DirtyFlagsComponent::Transform)) {
      recomputeLocalTransform(EntityHandle(registry_, entity), outWarnings);
      dirty.clear(DirtyFlagsComponent::Transform);
    }
    if (dirty.test(DirtyFlagsComponent::WorldTransform)) {
      recomputeWorldTransform(EntityHandle(registry_, entity));
      dirty.clear(DirtyFlagsComponent::WorldTransform);
    }
  }

  // 5. Shapes (only Shape-dirty entities)
  for (auto [entity, dirty] : dirtyView.each()) {
    if (dirty.test(DirtyFlagsComponent::Shape)) {
      ShapeSystem().createComputedPath(EntityHandle(registry_, entity));
      dirty.clear(DirtyFlagsComponent::Shape);
    }
  }

  // 6. Paint (only Paint-dirty entities)
  for (auto [entity, dirty] : dirtyView.each()) {
    if (dirty.test(DirtyFlagsComponent::Paint)) {
      PaintSystem().resolvePaint(EntityHandle(registry_, entity));
      dirty.clear(DirtyFlagsComponent::Paint);
    }
  }

  // 7. Filters (only Filter-dirty entities)
  for (auto [entity, dirty] : dirtyView.each()) {
    if (dirty.test(DirtyFlagsComponent::Filter)) {
      FilterSystem().resolveFilter(EntityHandle(registry_, entity));
      dirty.clear(DirtyFlagsComponent::Filter);
    }
  }

  // 8. Render instances (only RenderInstance-dirty entities)
  for (auto [entity, dirty] : dirtyView.each()) {
    if (dirty.test(DirtyFlagsComponent::RenderInstance)) {
      updateRenderInstance(EntityHandle(registry_, entity));
      dirty.clear(DirtyFlagsComponent::RenderInstance);
    }
  }

  // Clean up: remove DirtyFlagsComponent from fully clean entities
  for (auto [entity, dirty] : dirtyView.each()) {
    if (dirty.flags == DirtyFlagsComponent::None) {
      registry_.remove<DirtyFlagsComponent>(entity);
    }
  }
}
```

### Mutation Entry Points

Each DOM mutation API marks the appropriate dirty flags and propagates:

#### `SVGElement::setAttribute(name, value)`

```cpp
void SVGElement::setAttribute(const XMLQualifiedNameRef& name,
                              std::string_view value) {
  // ... existing attribute storage ...

  if (name == "style") {
    markDirty(DirtyFlagsComponent::StyleCascade);
    propagateStyleDirtyToDescendants();
  } else if (name == "class") {
    markDirty(DirtyFlagsComponent::StyleCascade);
    propagateStyleDirtyToDescendants();
  } else if (name == "transform") {
    markDirty(DirtyFlagsComponent::Transform);
    propagateWorldTransformDirtyToDescendants();
  } else if (isLayoutAttribute(name)) {
    markDirty(DirtyFlagsComponent::LayoutCascade);
  } else if (isShapeAttribute(name)) {
    markDirty(DirtyFlagsComponent::Shape | DirtyFlagsComponent::RenderInstance);
  } else if (isPresentationAttribute(name)) {
    markDirty(DirtyFlagsComponent::StyleCascade);
    if (isInheritedProperty(name)) {
      propagateStyleDirtyToDescendants();
    }
  }
}
```

#### `SVGElement::updateStyle(declarations)`

```cpp
void SVGElement::updateStyle(const css::Declaration& decl) {
  // ... existing style update ...
  markDirty(DirtyFlagsComponent::StyleCascade);
  propagateStyleDirtyToDescendants();
}
```

#### Tree Mutations

```cpp
void SVGElement::appendChild(SVGElement child) {
  // ... existing tree mutation ...
  markFullRebuildRequired();  // Draw order changed
}
```

### Propagation Helpers

```cpp
/// Mark this entity dirty with the given flags.
void SVGElement::markDirty(uint16_t flags) {
  auto& dirty = handle_.get_or_emplace<DirtyFlagsComponent>();
  dirty.flags |= flags;

  // Notify composited renderer if present
  if (auto* compositor = registry_.ctx().find<CompositedRenderer*>()) {
    (*compositor)->markEntityDirty(handle_.entity());
  }
}

/// Propagate Style dirty to all descendants (for inherited property changes).
void SVGElement::propagateStyleDirtyToDescendants() {
  TreeComponent::forEachDescendant(handle_, [](EntityHandle desc) {
    auto& dirty = desc.get_or_emplace<DirtyFlagsComponent>();
    dirty.mark(DirtyFlagsComponent::Style | DirtyFlagsComponent::Paint
               | DirtyFlagsComponent::RenderInstance);
  });
}

/// Propagate WorldTransform dirty to all descendants.
void SVGElement::propagateWorldTransformDirtyToDescendants() {
  TreeComponent::forEachDescendant(handle_, [](EntityHandle desc) {
    auto& dirty = desc.get_or_emplace<DirtyFlagsComponent>();
    dirty.mark(DirtyFlagsComponent::WorldTransform
               | DirtyFlagsComponent::RenderInstance);
  });
}
```

### Integration with Composited Renderer

The composited renderer already tracks per-layer dirty state. The incremental invalidation
system feeds into it naturally:

```
DOM mutation
  └─→ markDirty(flags) on affected entities
        └─→ CompositedRenderer::markEntityDirty(entity)
              └─→ LayerMembershipComponent → layer ID
                    └─→ layer.dirty = true

Next render:
  └─→ createComputedComponents() — only recomputes dirty entities
  └─→ CompositedRenderer::renderFrame()
        └─→ rasterizeLayer() — only dirty layers
        └─→ composeLayers() — all layers (fast blit)
```

This means a single-element style change results in:
1. Recompute style for ~1 entity (or N descendants if inherited)
2. Re-rasterize ~1 layer
3. Compose all layers (cheap)

vs. today's: recompute everything, re-rasterize everything.

### Spatial Index Updates

The spatial grid (`SpatialGrid` in the interactivity system) needs updating when
element geometry or transforms change. Elements with `WorldTransform` or `Shape` dirty
flags need their spatial grid entries updated:

```cpp
// After recomputing world transforms and shapes:
for (auto [entity, dirty] : dirtyView.each()) {
  if (dirty.test(DirtyFlagsComponent::WorldTransform | DirtyFlagsComponent::Shape)) {
    spatialGrid_.update(entity, getWorldBounds(entity));
  }
}
```

This is `O(k)` where k is the number of changed entities, vs. the current `O(n)` full
rebuild.

## Implementation Plan

### Phase 1: DirtyFlagsComponent and Mutation Hooks

Add the `DirtyFlagsComponent` and wire it into mutation entry points.

- [ ] Create `DirtyFlagsComponent` in `donner/svg/components/`
- [ ] Add `markDirty()` helper to `SVGElement`
- [ ] Wire `setAttribute()`, `setStyle()`, `updateStyle()`, `trySetPresentationAttribute()`
  to set appropriate dirty flags
- [ ] Wire tree mutations (`appendChild`, `removeChild`, `insertBefore`, `replaceChild`)
  to set `fullRebuildRequired_`
- [ ] Wire `SVGGeometryElement::invalidate()` to set `Shape` dirty
- [ ] Tests: verify dirty flags are set correctly for each mutation type

**No behavior change yet** — `createComputedComponents()` still does full recomputation
but the flags are being tracked.

### Phase 2: Dirty Propagation

Implement cascading invalidation for inherited properties and transforms.

- [ ] `propagateStyleDirtyToDescendants()` — walk tree, set `Style` dirty on descendants
- [ ] `propagateWorldTransformDirtyToDescendants()` — walk tree, set `WorldTransform` dirty
- [ ] Property inheritance classification — build a table of which CSS properties are
  inherited vs. non-inherited, used to decide whether to cascade
- [ ] Tests: verify propagation reaches correct descendants, stops at correct boundaries

### Phase 3: Selective Style Recomputation

Modify `StyleSystem` to skip clean entities.

- [ ] `StyleSystem::computeAllStyles()` checks `DirtyFlagsComponent::Style` and skips
  entities without it
- [ ] First-render path: when no `DirtyFlagsComponent` exists (first render), compute
  all styles (backwards compatible)
- [ ] Tests: verify that after a single-element style change, only that element (and
  inheriting descendants) are recomputed
- [ ] Correctness test: full recomputation output == incremental output (pixel-perfect)

### Phase 4: Selective Layout and Transform Recomputation

Modify `LayoutSystem` to skip clean entities.

- [ ] `LayoutSystem::instantiateAllComputedComponents()` skips entities without
  `Layout` or `Transform` dirty flags
- [ ] World transform accumulation respects dirty flags — only recompute from the
  highest dirty ancestor downward
- [ ] Tests: verify single-element transform change only recomputes that subtree

### Phase 5: Selective Shape, Paint, and Filter Recomputation

Complete the incremental pipeline for remaining systems.

- [ ] `ShapeSystem::createComputedPaths()` skips non-Shape-dirty entities
- [ ] `PaintSystem` skips non-Paint-dirty entities
- [ ] `FilterSystem` skips non-Filter-dirty entities
- [ ] Render instance update only for RenderInstance-dirty entities
- [ ] End-to-end test: DOM mutation → incremental recompute → render → pixel-perfect match

### Phase 6: Composited Renderer Integration

Connect incremental invalidation to the layer system.

- [ ] `markDirty()` automatically calls `CompositedRenderer::markEntityDirty()` when
  a compositor is active
- [ ] Verify: single-element mutation → single dirty layer → single layer re-rasterization
- [ ] Performance benchmark: measure speedup for single-element mutation in 100/500/1000
  element documents

### Phase 7: Spatial Index Incremental Updates

Update the spatial grid incrementally instead of rebuilding.

- [ ] After shape/transform recomputation, update only changed entities in the spatial grid
- [ ] Tests: hit testing remains correct after incremental updates

## Correctness Considerations

### Shadow Trees

Shadow trees (created for `<use>`, `<clipPath>`, `<mask>`, `<pattern>`, `<marker>`) are
cloned subtrees. When the source element changes, the shadow tree must be re-cloned.
This is handled by the `ShadowTree` dirty flag — when set, the shadow tree is torn down
and rebuilt from the (now-updated) source element.

Shadow tree invalidation is triggered when:
- The source element's subtree structure changes
- A `<use>` element's `href` attribute changes
- A `<clipPath>`/`<mask>`/`<pattern>` element's content changes

### Paint Server References

Fill and stroke can reference paint servers (`<linearGradient>`, `<radialGradient>`,
`<pattern>`) by ID. When a paint server's content changes, all elements referencing it
need `Paint` dirty. This requires a reverse reference map (paint server → referencing
elements), which the `PaintSystem` can maintain.

### Animation System

The animation system already updates `AnimatedValuesComponent` per tick. With incremental
invalidation, animation ticks should mark affected entities dirty:

```cpp
void AnimationSystem::applyAnimatedValue(Entity target, PropertyName prop, Value val) {
  // ... apply value ...
  auto& dirty = registry_.get_or_emplace<DirtyFlagsComponent>(target);
  dirty.mark(flagsForProperty(prop));
}
```

This replaces the current `invalidateRenderTree()` call in `SVGDocument::setTime()`, which
is a nuclear invalidation.

### First Render

On the first render, no `DirtyFlagsComponent` exists on any entity. The incremental path
must detect this and fall through to full recomputation. After the first render, all
entities are clean (no `DirtyFlagsComponent`), and subsequent mutations add flags
incrementally.

### Ordering Constraints

The 8-step pipeline has ordering dependencies:
1. Shadow trees must exist before styles can cascade into them
2. Styles must be computed before layout (size properties come from style)
3. Layout must be computed before transforms (viewBox affects transform)
4. Transforms must be computed before shapes (world-space bounds)
5. Shapes must be computed before paint (paint depends on geometry for patterns)

The incremental path must respect these ordering constraints. Within each step, only
dirty entities are processed, but steps still execute in order.

## Performance Model

For a document with N elements and k dirty elements:

| Operation | Full recomputation | Incremental |
|-----------|-------------------|-------------|
| Style resolution | O(N) | O(k + d) where d = inheriting descendants |
| Layout | O(N) | O(k) |
| Transform accumulation | O(N) | O(k + d) where d = descendants of changed |
| Shape decomposition | O(k_shapes) | O(k_shapes) (same — already per-element) |
| Paint resolution | O(N) | O(k) |
| Render instance update | O(N) | O(k) |
| Layer rasterization | O(dirty_layers × elements_per_layer) | Same |

For the common case (k=1, N=1000): ~1000x reduction in style/layout/paint work.

## Testing and Validation

- **Pixel-perfect correctness**: For every test in `renderer_tests` and `resvg_test_suite`,
  verify that incremental rendering after a mutation matches full recomputation rendering.
- **Dirty flag unit tests**: Each mutation type sets the correct flags and propagates
  correctly.
- **No-change fast path**: Verify that rendering without any mutation skips all recomputation.
- **Composited integration tests**: Single-element mutation → single dirty layer.
- **Performance benchmarks**: Measure per-frame time for incremental vs. full recomputation
  across document sizes (100, 500, 1000 elements).

## References

- [Chromium Style Invalidation](https://chromium.googlesource.com/chromium/src/+/HEAD/third_party/blink/renderer/core/css/invalidation/) — Browser CSS invalidation
- [Firefox Style Sharing](https://hacks.mozilla.org/2017/08/inside-a-super-fast-css-engine-quantum-css-aka-stylo/) — Stylo incremental restyle
- [SVG DOM Spec](https://svgwg.org/svg2-draft/types.html#SVGDOMOverview) — SVG mutation APIs
- [Composited Rendering Design](composited_rendering.md) — Layer dirty tracking (already implemented)
