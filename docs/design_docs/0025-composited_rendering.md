# Design: Composited Rendering

**Status:** Draft
**Author:** Claude Opus 4.6
**Created:** 2026-04-13

## Summary

When a user drags a shape in Donner's editor, today's pipeline re-rasterizes
the entire document every frame — `O(N)` work regardless of what changed. This
design introduces a **compositor** that caches rasterized content in off-screen
backing stores (layers), so frame-to-frame cost during interactive manipulation
is proportional to the number of *changed* layers, not total scene complexity.

The compositor sits between the ECS computed tree and the
`RendererInterface` backends. It is a shared, backend-agnostic component:
one implementation drives TinySkia, Skia, and Geode through a narrow
primitive set that each backend already (mostly) exposes. The critical
invariant is **pixel-identical correctness** — composited output must match
a full re-render within a precisely defined tolerance, verified
continuously in tests.

This design builds on [0005-incremental_invalidation](0005-incremental_invalidation.md)
(dirty-flag propagation from DOM mutations through the ECS pipeline) and
[0020-editor](0020-editor.md) (editor interaction model, mutation seam,
drag/select tools). It does *not* duplicate their work — it consumes
`DirtyFlagsComponent` as input and produces layer-level dirty notifications
as output.

## Goals

1. **60 fps dragging a single promoted shape in a 10,000-node scene on an
   Apple M1** (16.67 ms frame budget). Measured as time from composition
   transform update through composited frame completion, excluding
   display sync. Expected per-frame cost: ~5 ms on TinySkia (CPU blit
   + premultiply-aware path), ~3 ms on Skia (saveLayer + drawImage),
   <1 ms on Geode (GPU texture blit). Baseline today: >200 ms for
   10k nodes (full re-render).

2. **Pixel-identical composited output.** For every test in
   `renderer_tests` and `resvg_test_suite`, the composited path must
   produce output that matches the full-render path with `threshold=0,
   maxDiffPixels=0` (excluding documented AA-tolerance cases, which must
   be enumerated explicitly and capped at ≤2 LSB per channel, ≤0.1% of
   pixels).

3. **Correctness verification in every CI run.** A Bazel test flag
   (`--//donner/svg/compositor:dual_path_assertion=true`) enables the
   dual-path assertion that runs *both* paths (full render + composited)
   on a subset of the test suite and fails on any drift. This is always
   enabled in CI and compositor-specific test targets. It is NOT
   enabled in debug builds globally (the per-frame cost of two full
   renders would break interactive debugging). Developers can opt-in
   locally via `--config=compositor-debug`.

4. **No backend-specific compositor code.** The compositor is a single
   implementation in `donner::svg::compositor` that emits calls through
   `RendererInterface`. Backend-specific optimizations (Skia's
   `SkPicture` cache, Geode's GPU texture retention) live behind the
   existing `RendererInterface` virtuals or backend-internal caches, not
   in compositor logic.

5. **Editor integration via explicit promotion API.** The editor tells
   the compositor "this entity is a drag target" through a typed API
   (`CompositorController::promoteEntity`), not through CSS heuristics
   alone. Promotion is always reversible.

## Non-Goals

1. **Sub-layer partial re-rasterization** (dirty rectangles within a
   single layer). v1 re-rasterizes the entire layer when any element in
   it changes. Tile-level invalidation is future work.

2. **Automatic promotion heuristics** beyond the mandatory cases
   (opacity < 1, filter, mask, `isolation: isolate`). v1 promotes only
   what the editor explicitly requests or what SVG semantics force. CSS
   `will-change` is parsed but not acted on until v2.

3. **Concurrent/parallel layer rasterization.** Layers are rasterized
   sequentially on the calling thread. Thread-pool rasterization is a
   follow-up optimization.

4. **Animation-driven promotion.** SMIL/CSS animation does not
   auto-promote elements. The animation system marks dirty flags per
   [0005](0005-incremental_invalidation.md); the compositor consumes
   those flags like any other mutation.

5. **Perspective transforms.** SVG2 does not define perspective. The
   compositor handles affine transforms only. If a future CSS
   `perspective` extension lands, the compositor falls back to full
   re-render for affected subtrees.

6. **Video or streaming content layers.** Layers contain static
   rasterized content only.

7. **Layer count optimization / automatic merging.** v1 does not merge
   adjacent layers to reduce composition cost. Manual promotion is the
   only mechanism.

## Terminology

| Term | Definition |
|------|-----------|
| **Layer** | An off-screen pixel buffer (backing store) containing the rasterized content of one or more elements. Identified by a `LayerId`. |
| **Composition tree** | A flat, draw-order-sorted list of layers with associated transforms, opacity, blend mode, clip, and mask metadata. The compositor walks this list to produce the final frame. |
| **Promoted element** | An element that has been assigned its own layer (backing store). All other elements share the *root layer*. |
| **Root layer** | The default layer containing all non-promoted elements. Always exists, always layer 0. |
| **Damage region** | The axis-aligned bounding box (in device pixels) of all pixels that changed between frames. Used to limit the composition blit area. |
| **Layer promotion** | The act of assigning an element (and optionally its subtree) its own backing store. |
| **Demotion** | Returning a promoted element to the root layer, releasing its backing store. |
| **Compositor** | The component that manages layers, tracks damage, and composes layers into the final frame buffer. Does not rasterize — it delegates rasterization to `RendererInterface`. |
| **Fast path** | The composited rendering path: rasterize only dirty layers, compose all. |
| **Ground truth** | The full-render path: rasterize everything from scratch via `RendererDriver`. |

## High-Level Architecture

### Where the compositor sits

```
  DOM mutations
       │
       ▼
  DirtyFlagsComponent  (from 0005-incremental_invalidation)
       │
       ▼
  createComputedComponents()  ── selective recompute (style/layout/shape/paint)
       │
       ▼
  instantiateRenderTree()  ── RenderingInstanceComponent, sorted by drawOrder
       │
       ▼
  ┌────────────────────────────┐
  │  CompositorController      │  ◄── NEW: this design
  │  (donner/svg/compositor/)  │
  │                            │
  │  • Layer management        │
  │  • Dirty layer tracking    │
  │  • Composition pass        │
  └─────────┬──────────────────┘
            │ emits RendererInterface calls
            ▼
  ┌──────────────────────────┐
  │  RendererInterface       │
  │  (unchanged API)         │
  └──────┬──────┬──────┬─────┘
         │      │      │
    TinySkia  Skia   Geode
```

### Why a shared compositor, not backend-specific

Three arguments:

1. **Correctness is the hard part, not rasterization.** The compositor's
   job is deciding *what* to rasterize and *how* to compose. This logic
   (layer assignment, damage tracking, blend/clip/mask composition order)
   is identical regardless of whether pixels come from CPU or GPU. A
   single implementation means one correctness proof, not three.

2. **`RendererInterface` already abstracts rasterization.** The
   compositor needs: "rasterize these entities into this buffer" and
   "blit this buffer onto that buffer with this transform/opacity/blend."
   Both are expressible through existing `RendererInterface` primitives
   (`beginFrame`/`endFrame`, `drawImage`, `pushIsolatedLayer`,
   `setTransform`). No new virtuals are needed for v1.

3. **Backend-specific optimizations compose.** Skia can internally cache
   `SkPicture` recordings per layer. Geode can retain GPU textures
   across frames. These optimizations live *inside* the backend's
   `RendererInterface` implementation, invisible to the compositor. The
   compositor doesn't need to know — it just calls `drawImage` with a
   `RendererBitmap` and the backend decides how to upload it.

The risk is that the shared compositor cannot exploit backend-specific
composition hardware (e.g., Geode could compose layers via GPU texture
blits without CPU readback). This is acceptable for v1 because the
bottleneck is rasterization, not composition. v2 can add an optional
`RendererInterface::composeLayer()` fast path that GPU backends override.

### Key components

| Component | Location | Responsibility |
|-----------|----------|----------------|
| `CompositorController` | `donner/svg/compositor/CompositorController.h` | Public API. Manages layer lifecycle, processes dirty flags, orchestrates rasterization and composition. |
| `CompositorLayer` | `donner/svg/compositor/CompositorLayer.h` | Represents one layer: backing store (`RendererBitmap`), entity membership set, dirty flag, cached world-space bounds, opacity/blend/clip metadata. |
| `CompositionTree` | `donner/svg/compositor/CompositionTree.h` | Draw-order-sorted list of `CompositorLayer`s with composition metadata. Rebuilt when layer membership or z-order changes. |
| `DamageTracker` | `donner/svg/compositor/DamageTracker.h` | Computes dirty rectangles from layer dirty flags and transform changes. |
| `LayerMembershipComponent` | `donner/svg/components/LayerMembershipComponent.h` | ECS component on each entity: which `LayerId` it belongs to. Entities without this component are in the root layer. |
| `CompositorPromotionComponent` | `donner/svg/components/CompositorPromotionComponent.h` | ECS component marking an entity as explicitly promoted (by editor or by SVG semantics). Records promotion reason. |

### Data flow per frame

```
1. Editor drains command queue → marks DirtyFlagsComponent on entities

2. createComputedComponents() runs incremental recompute
   (style/layout/shape/paint for dirty entities only)

3. CompositorController::prepareFrame(registry)
   a. Check DirtyFlagsComponent on all entities
   b. For each dirty entity: look up LayerMembershipComponent → mark layer dirty
   c. If draw order changed (tree mutation): rebuild CompositionTree
   d. If promoted entity's subtree changed: update layer bounds

4. For each dirty layer:
   a. Create offscreen RendererInterface instance (createOffscreenInstance())
   b. Call RendererDriver::drawEntityRange() for entities in that layer
   c. Store resulting RendererBitmap in CompositorLayer

5. Composition pass:
   a. beginFrame() on main render target
   b. For each layer in draw order:
      - setTransform(layer.compositionTransform)
      - If layer has opacity < 1 or blend != Normal:
          pushIsolatedLayer(opacity, blendMode)
      - drawImage(layer.bitmap, layer.targetRect)
      - Pop isolated layer if pushed
   c. endFrame()
```

## Layer Promotion Policy

### Mandatory promotion (SVG semantics force isolation)

These elements *must* get their own layer because SVG compositing
semantics require group isolation. The compositor detects these by
inspecting `RenderingInstanceComponent`:

| Trigger | Detection | Reason |
|---------|-----------|--------|
| `opacity < 1.0` | `RenderingInstanceComponent::isolatedLayer == true` | Group opacity requires compositing the subtree as a unit, then applying opacity to the result. Without a layer, opacity would apply per-element. |
| `filter` applied | `RenderingInstanceComponent::resolvedFilter.has_value()` | Filter effects operate on the composited subtree result. |
| `mask` applied | `RenderingInstanceComponent::mask.has_value()` | Mask compositing requires an intermediate surface. |
| `mix-blend-mode != normal` | `isolatedLayer == true` (already triggered by RendererDriver) | Non-normal blend modes require group isolation. |
| `isolation: isolate` | `isolatedLayer == true` | Explicit CSS isolation. |

**v1 simplification:** Mandatory-promotion layers are *not* cached across
frames in v1. They are re-rasterized whenever any element in their subtree
is dirty, same as today. The compositor simply avoids re-rasterizing
*other* layers. This is conservative but correct — it matches the current
`pushIsolatedLayer`/`popIsolatedLayer` behavior exactly.

### Editor-requested promotion (drag targets)

The editor explicitly promotes elements via
`CompositorController::promoteEntity(entity, reason)`. Reasons:

| Reason | When | Backing store lifetime |
|--------|------|----------------------|
| `DragTarget` | User starts dragging an element. | Promoted on mouse-down, demoted on mouse-up + idle timeout (500 ms). |
| `WillChange` | CSS `will-change: transform` parsed. | Promoted when style is computed, demoted when property removed. |
| `EditorHint` | Editor marks element for performance (future). | Manual promote/demote. |

Editor-promoted layers are the primary optimization target. During a drag:

1. The dragged element's layer is *not* re-rasterized (only its
   composition transform changes).
2. The root layer is *not* re-rasterized (nothing in it changed).
3. Only the composition pass runs: blit root layer + blit drag layer
   at new position.

This reduces per-frame cost from `O(N)` rasterization to `O(L)` blits
where `L` is the number of layers (typically 2 during a drag).

### What does NOT get promoted

- Individual elements in a dragged group. The group's subtree shares one
  layer.
- Elements the user is not interacting with, regardless of complexity.
- Elements with only color/paint changes (re-rasterize in root layer).

## Damage Tracking

### Input: ECS change set

After `createComputedComponents()`, dirty entities still have their
`DirtyFlagsComponent` attached (cleared at end of frame). The
`DamageTracker` consumes these:

```cpp
struct DamageInfo {
  /// Layers that need re-rasterization.
  SmallVector<LayerId, 4> dirtyLayers;

  /// Union of old and new screen-space bounds of all dirty layers.
  /// Used to limit the composition blit region.
  Box2i
 damageRect;

  /// If true, the entire frame must be recomposed (layer order changed,
  /// layer added/removed, or viewport resized).
  bool fullRecompose = false;
};
```

### Computing dirty rectangles

For each entity with `DirtyFlagsComponent`:

1. **Look up `LayerMembershipComponent`** → `LayerId`. Mark that layer
   dirty.

2. **Compute screen-space damage.** The damage region is the union of:
   - The entity's *previous* screen-space bounds (cached on the layer).
   - The entity's *current* screen-space bounds (from
     `ComputedAbsoluteTransformComponent` + shape bounds).

   This handles the case where an element moves: the old position must
   be repainted (by the layer behind it) and the new position must be
   painted.

3. **Transform changes on promoted layers** are special: if *only* the
   `WorldTransform` flag is set on a promoted element (no `Style`,
   `Shape`, or `Paint` changes), the layer does **not** need
   re-rasterization — only the composition transform changes. This is
   the drag fast path.

### Viewport boundary crossings

When a promoted layer's screen-space bounds exit the viewport, the
compositor does not rasterize it (off-screen culling). When it re-enters,
the compositor must re-rasterize if the cached bitmap is stale. The
`CompositorLayer` tracks a `viewportIntersects` flag; transitions from
`false → true` force re-rasterization.

### Structural changes (z-order, tree mutations)

If `RenderTreeState::needsFullRebuild` is set (tree structure changed),
the compositor:

1. Rebuilds the `CompositionTree` from scratch.
2. Re-assigns all `LayerMembershipComponent`s.
3. Marks all layers dirty.
4. Falls through to full re-rasterization.

This is the conservative path. It is correct because it degrades to
today's behavior. Optimizing structural changes (e.g., only rebuilding
affected subtree layers) is future work.

## Composition Pass

The composition pass blits cached layer bitmaps onto the final render
target. It runs every frame, even when no layers are dirty (because the
viewport may have scrolled or the editor overlay needs redrawing).

### Layer ordering

Layers are sorted by the minimum `drawOrder` of their member entities.
This preserves SVG paint order.

### Composition operations per layer

For each layer in draw order:

```
1. setTransform(layer.compositionTransform)
   // compositionTransform = deviceFromWorld * layer.worldOffset
   // For non-promoted layers, this is identity.
   // For promoted layers during drag, this incorporates the drag delta.

2. If layer.opacity < 1.0 || layer.blendMode != Normal:
     pushIsolatedLayer(layer.opacity, layer.blendMode)

3. If layer.clipPath.has_value():
     pushClip(layer.resolvedClip)

4. If layer.mask.has_value():
     pushMask(layer.maskBounds)
     // render mask content from cached mask layer
     transitionMaskToContent()

5. drawImage(layer.bitmap, layer.targetRect)

6. Pop mask/clip/isolated layer in reverse order
```

### Filters during composition

Filters are applied during *layer rasterization*, not during composition.
When an element with a filter is rasterized into its layer, the filter
pipeline runs as part of that rasterization (via `pushFilterLayer` /
`popFilterLayer` on the offscreen `RendererInterface`). The compositor
blits the filter's *output* — no filter logic in the composition pass.

### Clip-path during composition

Clip-paths that apply to a promoted element are resolved during layer
rasterization (the offscreen pass clips to the element's clip-path).
Clip-paths that apply to a *group* containing both promoted and
non-promoted elements are handled by clipping the composition blit — the
compositor applies the group's clip to the blitted layer bitmap.

If a clip-path references geometry that *itself* changes (e.g., an
animated clip), the compositor marks the affected layer dirty for
re-rasterization. Clip-path-only changes are not compositable — the layer
must be re-rasterized because the clip boundary changed.

## Correctness Analysis

This is the critical section. For each rendering feature, we enumerate
whether the composited fast path produces identical output, and if not,
what safeguard prevents drift.

### Affine transforms

**Claim: exact.** Promoted layers cache rasterized content in the layer's
local coordinate space. During composition, the layer bitmap is blitted
with the layer's world transform applied via `setTransform()` +
`drawImage()`. For pure translation (the drag case), this is a
pixel-aligned blit — no resampling, no error.

For rotation/scale composition transforms, `drawImage()` resamples the
cached bitmap. This produces output *different* from rasterizing the
vector geometry directly at the final transform because of
rasterization-then-transform vs. transform-then-rasterize ordering.

**Safeguard:** The compositor rasterizes promoted layers at a fixed
transform (the layer's world transform at promotion time). If the
composition transform diverges from identity by more than pure
translation, the compositor marks the layer for re-rasterization at the
new transform. During drag, the editor constrains mutations to
translation-only transforms. Rotation/scale drags trigger
re-rasterization every frame (degrading to full-render cost for that
layer, but not for the rest of the scene).

**Conservative rule:** If `compositionTransform` is not a pure
translation (checked via `Transform2d::isTranslation()`), re-rasterize
the layer. This preserves pixel-identical output at the cost of losing
the fast path for non-translational drags.

### Perspective transforms

**Not applicable.** SVG2 does not define perspective transforms. If CSS
`perspective` or `transform: perspective(...)` is encountered, the parser
ignores it (falls through to `none`). No compositor path needed.

### Opacity

**Claim: exact for promoted layers, under a constraint.**

Group opacity (opacity < 1 on a `<g>` element) requires compositing the
group as a unit, then applying opacity. The compositor handles this by
rasterizing the group into its layer at full opacity, then applying
opacity during the composition blit via `pushIsolatedLayer(opacity)`.
This matches `RendererDriver`'s existing behavior exactly because
`RendererDriver` already uses `pushIsolatedLayer` for the same purpose.

**Constraint:** If opacity changes between frames (e.g., animated
opacity), the compositor must re-compose with the new opacity value.
The layer bitmap does *not* need re-rasterization — only the composition
opacity parameter changes. `DirtyFlagsComponent::RenderInstance`
triggers a recompose but not a re-rasterize when only opacity changed.

**Edge case: opacity on a promoted element that is also a drag target.**
The drag delta is a translation on the composition transform, and
opacity is applied during composition. These compose correctly because
opacity is multiplicative and translation-invariant.

### Clip-path

**Claim: exact for static clip-paths. Conservative fallback for dynamic
clip-paths.**

Static clip-paths (geometry doesn't change between frames) are applied
during layer rasterization. The layer bitmap includes the clip — no clip
logic during composition. This is exact because the clip is applied to
vector geometry before rasterization, identical to the full-render path.

If a clip-path's geometry changes (shape mutation, animated clip),
`DirtyFlagsComponent::Shape` on the clip-path entity triggers
re-rasterization of all layers that reference it. This is detected via a
reverse-reference map from clip-path entities to their consumers
(analogous to the paint-server reverse map in
[0005-incremental_invalidation](0005-incremental_invalidation.md)).

**Clip-path on a group spanning multiple layers:** If a `<g clip-path>`
contains both promoted and non-promoted children, the clip must apply to
the *composed* result. The compositor handles this by:

1. Composing the group's layers into a temporary buffer.
2. Applying the clip to the temporary buffer.
3. Blitting the clipped result to the final target.

This adds one temporary buffer allocation. If this proves too expensive,
the fallback is to de-promote children within clipped groups (the
group shares a single layer). v1 uses the de-promotion fallback.

### Mask

**Claim: conservative fallback.**

Masks require rendering the mask content, converting to luminance, and
applying as alpha. This is inherently a per-rasterization operation. The
compositor does *not* attempt to cache mask bitmaps separately.

**Rule:** Elements with masks always re-rasterize when marked dirty.
The mask is applied during layer rasterization via the existing
`pushMask` / `transitionMaskToContent` / `popMask` sequence. This is
identical to today's full-render behavior.

**Optimization opportunity (deferred):** Cache the mask bitmap separately
and re-apply during composition when only the masked content changes (not
the mask itself). This requires tracking mask dependencies, which adds
complexity for marginal gain in v1.

### Filter effects

**Claim: conservative fallback.**

Filter effects (`<filter>`) operate on the rasterized content of their
input. The filter graph executes during layer rasterization via
`pushFilterLayer` / `popFilterLayer`. The compositor blits the
filter's output — it does not re-run filters during composition.

**Rule:** If any element within a filtered subtree is dirty, the entire
layer containing the filter host is re-rasterized (because the filter's
input changed). Filter-only changes (e.g., `feGaussianBlur` stdDeviation
animation) also trigger re-rasterization.

**Why not cache filter output separately?** Filter graphs can have
multiple inputs (`SourceGraphic`, `BackgroundImage`, other primitives).
`BackgroundImage` depends on content *behind* the filtered element,
which lives in a different layer. Caching filter output across layers
requires solving cross-layer dependency tracking — too complex for v1.

### mix-blend-mode

**Claim: exact.** Non-normal blend modes already require isolated layer
compositing in SVG. The compositor mirrors this: elements with
`mix-blend-mode != normal` are promoted to their own layer (mandatory
promotion), rasterized in isolation, and blitted with the specified blend
mode during composition via `pushIsolatedLayer(opacity, blendMode)`.
This matches `RendererDriver`'s existing behavior.

**Edge case: `mix-blend-mode` on an element within a promoted drag
layer.** The blend mode is applied during layer rasterization (the
element is rasterized into the drag layer with its blend mode against
other elements in that layer). During composition, the drag layer is
blitted with `Normal` blend mode (unless the drag target itself has a
non-normal blend mode, in which case the layer is blitted with that
mode). This matches ground truth because the same isolation boundary
exists in both paths.

### Z-order changes

**Claim: conservative fallback.**

Any tree mutation that changes z-order (insertion, deletion, reordering)
sets `RenderTreeState::needsFullRebuild`. The compositor responds by:

1. Tearing down the `CompositionTree`.
2. Rebuilding layer membership from scratch.
3. Marking all layers dirty.
4. Falling through to full re-rasterization + full recomposition.

This is correct because it degrades to today's behavior. It is also rare
during interactive editing — drag operations change transforms, not tree
structure.

### Inherited properties (currentColor, font-size units, text inheritance)

**Claim: handled by incremental invalidation, not by the compositor.**

Inherited property changes cascade `DirtyFlagsComponent::Style` to
all descendants (per [0005-incremental_invalidation](0005-incremental_invalidation.md),
§ Invalidation Propagation Rules). After `createComputedComponents()`
runs, affected entities have updated `ComputedStyleComponent` values.
The compositor sees these entities as dirty and re-rasterizes their
layers.

**Specific cases:**

- **`currentColor` change on an ancestor:** Cascades `Style` dirty to all
  descendants. All layers containing those descendants are re-rasterized.
  Correct because the compositor always re-rasterizes dirty layers with
  the latest computed styles.

- **`font-size` change (affects `em`/`ex` units in descendants):**
  Cascades `Style` + `Layout` dirty. Same path as above.

- **Text inheritance (`font-family`, `text-anchor`, etc.):** Same path.
  The compositor is downstream of style resolution and sees only the
  final computed values.

**No compositor-specific logic needed.** The correctness argument is:
the compositor never uses stale computed styles because it only
rasterizes after `createComputedComponents()` completes.

### Overlapping transformed regions

**Claim: exact for non-overlapping layers. Conservative fallback for
overlapping promoted layers.**

When two promoted layers overlap in screen space:

- If neither is dirty, the cached composition is correct (nothing
  changed).
- If one is dirty and re-rasterized, the composition blits in draw
  order. Because each layer's content is independently rasterized
  against a transparent background, the composition order matches the
  full-render paint order. This is exact.
- If a drag moves a promoted layer to *newly overlap* with another
  promoted layer, the composition is still exact because `drawImage`
  with the correct blend mode (default: `SrcOver`) produces the same
  result as if the elements were painted in that order.

**Edge case: overlapping promoted layers with `BackgroundImage` filter
input.** `BackgroundImage` captures content behind the current element.
In a composited path, "behind" means previously composed layers — but if
those layers were cached and not re-rasterized, the `BackgroundImage`
may be stale.

**Rule:** Elements using `BackgroundImage` or `BackgroundAlpha` filter
inputs are *never* compositable. They force their layer and all layers
they depend on to re-rasterize. Detection: scan `FilterGraph` inputs
during promotion for `BackgroundImage`/`BackgroundAlpha` references.

### Summary table

| Feature | Fast-path correctness | Mechanism |
|---------|----------------------|-----------|
| Translation transform | Exact | Pixel-aligned blit |
| Rotation/scale transform | Exact (re-rasterize) | Non-translation detected, layer re-rasterized |
| Perspective | N/A | SVG2 doesn't define it |
| Opacity (static) | Exact | Applied during composition blit |
| Opacity (animated) | Exact | Recompose with new value, no re-rasterize |
| Clip-path (static) | Exact | Applied during layer rasterization |
| Clip-path (animated) | Exact (re-rasterize) | Dirty flag triggers re-rasterize |
| Clip-path (cross-layer) | Exact (de-promote) | Children within clipped group share one layer |
| Mask | Exact (re-rasterize) | Always re-rasterize masked layers |
| Filter | Exact (re-rasterize) | Always re-rasterize filtered layers |
| mix-blend-mode | Exact | Blended during composition, same as RendererDriver |
| Z-order change | Exact (full rebuild) | Falls through to full re-render |
| Inherited properties | Exact | Dirty flags cascade, layers re-rasterize |
| Overlapping layers | Exact | SrcOver composition matches paint order |
| BackgroundImage filter | Exact (never composite) | Disables fast path for affected elements |
| **Hit testing during drag** | **Adjusted** | Compositor transform applied to hit-test path |
| **Layer bitmap sizing** | **Exact** | Ink rectangle (stroke + markers + filter expansion) |
| **Markers** | **Exact (de-promote or re-rasterize)** | Marker subtrees included in layer entity range |
| **`feImage` cross-layer ref** | **Exact (de-promote)** | Fragment refs targeting other layers force de-promotion |
| **Nested isolation groups** | **Exact (de-promote)** | Promoted elements inside isolation scope share parent layer |
| **`<use>` shadow trees** | **Exact (re-rasterize)** | Dirty cascade from target to shadow instances |
| **Pattern/gradient `currentColor`** | **Exact (re-rasterize)** | Reverse-ref map (0005) triggers re-rasterize on change |
| **Deferred-pop stack integrity** | **Exact (de-promote)** | Groups straddling layer boundaries share one layer |

### Hit testing during drag

**Problem:** During drag, the promoted element's composition transform
is applied only at composition time — it is NOT reflected in the ECS
`AbsoluteTransformComponent`. Hit-testing uses world-space transforms
from ECS, so clicking the visual (dragged) position misses, and
clicking the original (pre-drag) position hits an invisible element.

**Safeguard:** The compositor exposes `compositionTransformOf(entity)`
which the hit-test path composes with the entity's world transform.
During drag, the editor's hit-test code queries the compositor for the
active composition transform and applies it to the hit-test point
(inverse transform the pointer, or forward-transform the element
bounds). When compositing is disabled or the entity is not promoted,
this returns identity.

### Layer bitmap sizing

**Problem:** The layer bitmap must be large enough to contain the
promoted element's full visual extent, including:

- Stroke width and stroke alignment (`stroke-linejoin: miter` can
  extend well beyond the geometry bbox).
- Marker subtrees (markers extend beyond the path they decorate).
- Filter expansion (`feMorphology`, `feGaussianBlur`, `feOffset`
  expand the filter region beyond the geometry bbox).
- `overflow: visible` content on `<svg>`, `<symbol>`, `<pattern>`.

**Rule:** Layer bitmap dimensions = ink rectangle (computed bounding
box including all of the above), expanded by the filter primitive
subregion if a filter is active. The compositor queries
`ComputedSizedElementComponent` for the base bbox, then applies
filter region expansion from `ComputedFilterComponent`. If the ink
rectangle exceeds the viewport, it is clipped to the viewport (content
outside the viewport is not visible and does not need rasterization).

### Markers

**Problem:** Marker subtrees (`<marker>` definitions in `<defs>`)
are instantiated during rasterization at each marker position along
the decorated path. The marker definition entity is in `<defs>` and
may be outside the promoted element's entity range.

**Safeguard:** `drawEntityRange()` does not need marker definition
entities in-range — marker instantiation is handled by
`RenderingContext` which resolves marker references from `<defs>`
regardless of entity range. The layer rasterization step calls
`drawEntityRange()` with the promoted element's range, and the
renderer resolves marker references globally. No special handling
needed.

However, if the marker definition itself is modified (e.g., a
`<marker>` element's child changes), the dirty flag must cascade to
all elements that reference that marker. This is handled by the
existing reverse-reference map (design doc 0005). The compositor
detects the dirty flag on the marker-using element and re-rasterizes
its layer.

### `feImage` cross-layer references

**Problem:** The `feImage` filter primitive with `href="#fragment"`
renders the referenced SVG element as the filter input. If the
referenced element is in a different compositor layer, the compositor
must ensure the referenced layer is rasterized before the filter
layer. Additionally, dragging the referenced element changes the
filter output without the filter's owning element having a dirty flag.

**Safeguard (v1, conservative):** If an element has an `feImage`
primitive whose `href` targets an element in a different compositor
layer, de-promote the `feImage`-owning element (refuse to keep it
in a separate layer). This ensures `feImage` always renders through
the full path. The detection is done at promotion time by scanning
the element's computed filter graph for `feImage` nodes with fragment
references. This is conservative but correct; v2 can add cross-layer
dependency tracking.

### Nested isolation groups

**Problem:** SVG2 and CSS Compositing Level 1 define isolation
groups: an element with `isolation: isolate` or `mix-blend-mode !=
normal` creates an isolated stacking context. Blend operations target
the nearest isolation group's backdrop, not the page root.

If a promoted element has `mix-blend-mode != normal` and its nearest
isolation ancestor is NOT in the same compositor layer, the blend
target is wrong — the element blends against the root layer's
content instead of the isolation group's content.

**Safeguard:** Elements inside an `isolation: isolate` ancestor that
also participate in non-normal blending are not separately promotable.
`promoteEntity()` checks the element's ancestor chain for isolation
context boundaries. If found, the element is added to the ancestor's
layer (if the ancestor is promoted) or promotion is refused.

For v1, this is a conservative de-promotion rule. The more common
case (element with `mix-blend-mode` that is NOT inside an explicit
isolation group) works correctly because the blend target is the page
root, which the root compositor layer represents.

### `<use>` shadow trees

**Problem:** `<use>` elements create shadow tree clones of their
target element. The shadow tree entities are distinct from the target
entities, but they inherit styles and may reference the same
resources (gradients, patterns, clip-paths) as the target.

**Safeguard:** Each `<use>` shadow tree instance is a separate set
of entities. If the `<use>` element is promoted, its shadow tree
entities are within the promoted entity range (they are children in
the ECS tree). If the `<use>` *target* element changes (not the
`<use>` element itself), the dirty flag cascades from the target
to all `<use>` elements referencing it via `ShadowTreeComponent`
dirty propagation. The compositor detects this cascade and
re-rasterizes the affected `<use>` element's layer.

### Pattern and gradient `currentColor` dependency

**Problem:** `currentColor` in gradient stops or pattern content
resolves to the `color` property of the element using the paint
server. If a promoted element's ancestor's `color` property changes,
the gradient/pattern output changes, but the paint server entity
itself may not have a dirty flag.

**Safeguard:** The reverse-reference map (design doc 0005) tracks
paint server → using element dependencies. When `color` changes on
an ancestor, `DirtyFlagsComponent::Paint` is set on descendant
elements that use `currentColor`-dependent paint servers. The
compositor detects this dirty flag and re-rasterizes. Until the
reverse-reference map is implemented, this is a known limitation:
`currentColor` changes in gradients/patterns may produce stale
layer bitmaps. Workaround: always re-rasterize layers that use
`currentColor`-dependent paint servers.

### Deferred-pop stack integrity

**Problem:** `RendererDriver` uses a deferred-pop stack for
push/pop pairs (clip, mask, isolation, filter). If a group element
that pushes state straddles a layer boundary (some children in layer
A, some in layer B), the push/pop nesting breaks.

**Safeguard:** The compositor's de-promotion rule is generalized:
any element whose rendering requires push/pop state set by an
ancestor outside its layer boundary MUST share the ancestor's layer.
This is enforced at promotion time by walking the ancestor chain
and checking for active clip-path, mask, filter, and isolation
contexts. If any ancestor between the promoted element and the
document root has such a context, the promoted element is added to
the ancestor's layer or promotion is refused.

In practice, this means elements inside `<g clip-path="...">`,
`<g filter="...">`, `<g mask="...">`, or `<g style="isolation:
isolate">` groups are promoted as part of the group, not
individually. This is already the natural behavior for the
editor's drag workflow (dragging a group promotes the group).

### Compositor-specific golden tests (expanded)

In addition to the base golden tests listed above, add these
SpecBot-recommended tests:

- Promoted element with `mix-blend-mode: multiply` inside
  `isolation: isolate` parent — verify blend target is correct.
- `feImage href="#fragment"` where fragment is in a different layer
  — verify de-promotion and correct rendering.
- Promoted `<path>` with `marker-mid` where marker definition
  is in `<defs>` — verify markers render correctly in layer.
- Promoted element with `overflow: visible` on ancestor `<svg>` —
  verify layer bitmap includes overflow content.
- `<use>` element promoted, target element modified — verify
  dirty cascade triggers re-rasterization.
- Promoted element with `fill: url(#gradient)` where gradient
  uses `currentColor` and ancestor `color` changes — verify
  re-rasterization.
- Deferred-pop edge case: promote a `<rect>` inside a
  `<g clip-path="..." mask="...">` group — verify the group
  constraint forces shared layer.

## Backend Integration

### Minimum primitive set

The compositor needs these `RendererInterface` primitives:

| Primitive | Used for | TinySkia | Skia | Geode |
|-----------|----------|----------|------|-------|
| `createOffscreenInstance()` | Layer rasterization into separate buffer | ✅ | ✅ | ❌ (returns `nullptr`; needs shared-device constructor) |
| `beginFrame()` / `endFrame()` | Frame lifecycle for offscreen and main targets | ✅ | ✅ | ✅ |
| `drawImage()` | Blit layer bitmap to main target | ✅ | ✅ | ✅ (`GeodeImagePipeline`) |
| `setTransform()` | Composition transform for layer blit | ✅ | ✅ | ✅ |
| `pushIsolatedLayer()` / `popIsolatedLayer()` | Opacity/blend during composition | ✅ | ✅ | ✅ (opacity only; `MixBlendMode != Normal` pending) |
| `pushClip()` / `popClip()` | Clip during composition | ✅ | ✅ | ✅ (rect + polygon + path-mask clips) |
| `pushMask()` / `popMask()` | Mask during layer rasterization | ✅ | ✅ | ✅ (Phase 3c luminance mask compositing) |
| `pushFilterLayer()` / `popFilterLayer()` | Filter during layer rasterization | ✅ (via `FilterGraphExecutor`) | ✅ | ❌ (no-op stub) |
| `takeSnapshot()` | Extract layer bitmap for cross-layer composition | ✅ | ✅ | ✅ |
| `RendererDriver::drawEntityRange()` | Rasterize a subset of entities | ✅ | ✅ | ✅ |

**Geode v1 participation:** Geode can participate in compositor testing
for the translation-only drag path on scenes without filters or
non-normal blend modes. Compositor tests for Geode should be gated
behind a feature check that excludes filter/blend-mode cases.

### `RendererBitmap` adapter and alpha model (resolved OQ#1)

The compositor uses `drawImage(ImageResource, ImageParams)` to blit
layer bitmaps. The `RendererBitmap` from `takeSnapshot()` must be
convertible to an `ImageResource`. This conversion has correctness and
performance pitfalls that must be resolved before implementation.

**Problem 1: Premultiplied alpha mismatch.** `takeSnapshot()` returns
pixel data in **premultiplied** RGBA format (TinySkia's internal format;
Skia's surface uses the `MakeN32` alpha type). `drawImage()` on all
three backends assumes its `ImageResource` input contains **straight
(non-premultiplied)** alpha and re-premultiplies on ingestion. Feeding
premultiplied data through this path double-premultiplies, darkening all
semi-transparent content. Additionally, the premultiply→unpremultiply→
premultiply roundtrip introduces **±1 LSB error per channel**, violating
the `threshold=0` correctness goal.

**Problem 2: Color type mismatch.** Skia's `MakeN32` uses the platform's
native 32-bit color type (`kBGRA_8888` on desktop Linux), but `drawImage`
declares `kRGBA_8888`. If these differ, red and blue channels swap.

**Problem 3: Allocation cost.** The generic `takeSnapshot()` → `drawImage()`
path involves 2–4 full-resolution copies per layer blit (66–132 MB of
allocation per frame at 4K for 2 layers). At 60 fps, this produces ~4–8
GB/s of heap allocation churn.

**Resolution: Two-tier adapter.**

*Tier 1 (v1, all backends):* Add an `AlphaType` enum and `colorType`
field to `RendererBitmap`. Add a `premultiplied` flag to `ImageResource`.
When `drawImage` receives premultiplied data, skip the premultiply step.
Normalize `takeSnapshot()` output to canonical premultiplied RGBA on all
backends. This eliminates the correctness bugs and halves the allocation
cost (removing the premultiply copy).

```cpp
enum class AlphaType : uint8_t { Premultiplied, Unpremultiplied };
struct RendererBitmap {
  Vector2i dimensions;
  std::vector<uint8_t> pixels;
  std::size_t rowBytes = 0;
  AlphaType alphaType = AlphaType::Premultiplied;
};
```

*Tier 2 (v2, per-backend fast path):* Add a `LayerHandle` abstraction
to `RendererInterface` that bypasses `RendererBitmap` entirely:

```cpp
struct LayerHandle {
  virtual ~LayerHandle() = default;
};
virtual std::unique_ptr<LayerHandle> retainCurrentFrame() { return nullptr; }
virtual void drawLayer(const LayerHandle& handle, const ImageParams& params) {}
```

Skia implements `retainCurrentFrame()` via
`surface->makeImageSnapshot()` (zero-copy COW `SkImage`). Geode retains
the GPU texture handle. TinySkia accesses the offscreen `frame_` pixmap
directly via `PixmapView` (no `takeSnapshot()` copy). Each backend gets
its native fast path.

*TinySkia-specific optimization:* For TinySkia, the compositor can add
an internal `blitFrom(const RendererTinySkia& source, ...)` method that
composites the offscreen `frame_` pixmap directly via `Painter::drawPixmap`
on the premultiplied `PixmapView`, eliminating all copies. This does not
change `RendererInterface` — it is a TinySkia-internal friend function.

**No new `RendererInterface` virtuals in v1.** The tier-1 adapter changes
only existing struct fields. The tier-2 `LayerHandle` virtuals are a v2
addition.

### Skia-specific considerations

Skia's `SkPicture` recording could cache draw commands per layer and
replay them without re-traversing the ECS. However, this is only
valuable if combined with sub-layer dirty rectangles (Phase 4): for
whole-layer re-rasterization, the compositor's bitmap cache already
skips both ECS traversal and rasterization for clean layers, making
`SkPicture` redundant. Phase 3's Skia optimization is gated on Phase 4.

**`takeSnapshot()` → `drawImage()` path:** Skia's internal composition
(e.g., `popFilterLayer`, `popMask`, `endPatternTile`) uses
`surface->makeImageSnapshot()` → `canvas->drawImage()` — a zero-copy
path via copy-on-write `SkImage`. The compositor's v1 path goes through
`takeSnapshot()` (CPU readback) → `ImageResource` → `drawImage()`
(re-upload), which involves 4 full-resolution copies. This is acceptable
for v1 (bottleneck is rasterization, not composition), but v2 should add
a `LayerHandle` abstraction that Skia implements via `SkImage` COW.

**`saveLayer` allocation during composition:** `pushIsolatedLayer()` maps
to `saveLayer(nullptr, &paint)`, which allocates a full-canvas-sized
offscreen bitmap. During composition blits (one bitmap per layer), this
is wasteful — opacity and blend mode can be applied directly on the
`SkPaint` passed to `drawImageRect()`. The v1 impact is manageable
(sequential composition limits peak to 1 extra saveLayer at a time), but
the memory budget (§ Security) must account for it.

### Geode-specific considerations

Geode renders to GPU textures via `GeoSurface`. Layer bitmaps can be
retained as GPU textures across frames, avoiding the CPU readback path
(`takeSnapshot()` → `drawImage()`). In v1, the compositor goes through
the CPU readback path for simplicity. v2 can add a `LayerHandle`
abstraction to `RendererInterface` (see § v2 Layer Handle below).

**`createOffscreenInstance()` prerequisite:** Geode does not currently
override `createOffscreenInstance()` (returns `nullptr` from the base
class default). Implementation requires a shared-device constructor:
offscreen instances share the parent's `GeodeDevice` and pipeline
state objects (`GeodePipeline`, `GeodeGradientPipeline`,
`GeodeImagePipeline`) while maintaining their own `GeoEncoder` and
texture pair per render target. This matches the existing
`pushIsolatedLayer` pattern, which already creates new
encoder+texture pairs on the shared device. Pipeline state objects
are device-scoped in WebGPU and can be reused across any number of
render targets.

Geode's current `drawImage` implementation (`GeodeImagePipeline` +
`GeodeTextureEncoder::drawTexturedQuad`) is already designed for
pre-uploaded `wgpu::Texture` handles. The v2 `LayerHandle` adapter
would skip the CPU readback and pass the GPU texture directly via
`surface->makeImageSnapshot()` (Skia) or texture handle retention
(Geode).

## Editor Interaction Model

### API surface

```cpp
namespace donner::svg::compositor {

enum class PromotionReason : uint8_t {
  DragTarget,     ///< Editor drag in progress.
  WillChange,     ///< CSS will-change: transform.
  EditorHint,     ///< Manual editor hint.
  Mandatory,      ///< SVG semantics (opacity, filter, mask, blend).
};

/// Public compositor API consumed by the editor.
class CompositorController {
public:
  explicit CompositorController(Registry& registry);

  /// Promote an entity to its own layer for the given reason.
  /// If already promoted, adds the reason (reasons accumulate).
  void promoteEntity(Entity entity, PromotionReason reason);

  /// Remove a promotion reason. Layer is demoted when no reasons remain.
  void demoteEntity(Entity entity, PromotionReason reason);

  /// Update the composition-time transform for a promoted layer.
  /// This is the fast path: no re-rasterization, only changes the
  /// blit transform during composition.
  void setLayerCompositionTransform(Entity entity,
                                    const Transform2d& compositionTransform);

  /// Prepare the frame: consume dirty flags, mark dirty layers.
  /// Must be called after createComputedComponents().
  DamageInfo prepareFrame();

  /// Rasterize dirty layers and compose the final frame.
  /// Calls into RendererInterface.
  void renderFrame(RendererInterface& renderer,
                   const RenderViewport& viewport);

  /// Query whether compositing is active (at least one promoted layer).
  [[nodiscard]] bool isActive() const;
};

}  // namespace donner::svg::compositor
```

### Editor drag workflow

```
1. Mouse down on element E:
   editor calls CompositorController::promoteEntity(E, DragTarget)
     → CompositorLayer created for E
     → E's content rasterized into layer bitmap
     → Root layer re-rasterized WITHOUT E (E is now in its own layer)

2. Mouse move (drag delta = dx, dy):
   editor calls CompositorController::setLayerCompositionTransform(
       E, Transform2d::Translate(dx, dy))
     → No rasterization. Only composition transform updated.
     → renderFrame() blits root layer + E's layer at offset (dx, dy)
     → Cost: O(L) blits, L = 2

3. Mouse up:
   editor calls EditorApp::applyMutation(TransformSet{E, finalTransform})
     → DirtyFlagsComponent::Transform set on E
     → createComputedComponents() updates E's world transform
     → CompositorController demotes E after idle timeout
     → E returns to root layer, root layer re-rasterized with E at
       final position

4. Idle timeout (500 ms after mouse up):
   editor calls CompositorController::demoteEntity(E, DragTarget)
     → Layer torn down, bitmap freed
     → Root layer marked dirty, re-rasterized with E
```

### Promotion during drag does not trigger full re-render

Key subtlety: when E is promoted (step 1), the root layer must be
re-rasterized *without* E. This means one frame of full-rasterization
cost at the start of the drag. Subsequent frames (step 2) are cheap.
Similarly, demotion (step 4) triggers one re-rasterization. The cost
model is:

- Drag start: 1× full rasterization (root layer without E + E's layer)
- Drag frames: 0× rasterization (composition only)
- Drag end: 1× full rasterization (root layer with E at final position)

For a 10,000-element scene at ~200 ms per full render, the drag start
and end each cost ~200 ms (one frame drop), but all intervening drag
frames are <1 ms (composition only). This is the target behavior.

## Verification Strategy

### Pixel-diff in tests

Every `renderer_tests` and `resvg_test_suite` test case gains a second
execution mode: render via the composited path with a trivially promoted
root layer (all elements in one layer). Compare against the ground-truth
full-render. Threshold: `maxDiffPixels=0, threshold=0`.

This runs in CI on every PR. It catches any compositor bug that produces
different output from the full render.

### Dual-path debug assertion

When enabled via the Bazel flag
`--//donner/svg/compositor:dual_path_assertion=true` (or
`--config=compositor-debug`), `CompositorController::renderFrame()`
runs *both* the composited path and a full re-render, then compares
the results pixel-by-pixel. On mismatch, it:

1. Logs the differing pixel coordinates and values.
2. Writes both bitmaps to a temp directory for inspection.
3. Fires `UTILS_RELEASE_ASSERT_MSG` with a descriptive message.

This is expensive (2× render cost) and is always enabled in CI
compositor test targets. It is NOT enabled globally in debug builds
(the per-frame cost would make interactive debugging unusable).
Developers opt-in locally via `--config=compositor-debug`.

**Snap to integer pixels:** During composition, all translation
offsets are snapped to integer device pixels before the blit. This
prevents sub-pixel filtering from introducing differences between
the composited and full-render paths. The snap is performed as
`round(offset)` in device-pixel coordinates.

### Property-test-style random scenes

A new test target (`compositor_fuzz_tests`) generates random SVG scenes
with:

- Random element count (1–500).
- Random transforms (translate, rotate, scale).
- Random opacity (0.0–1.0).
- Random clip-paths and masks.
- Random promotion of 1–5 elements.
- Random drag transforms on promoted elements.

For each scene:

1. Render via full path → bitmap A.
2. Render via composited path → bitmap B.
3. Assert A == B (within AA tolerance).

This runs as a long-running fuzzer, not in per-PR CI. It explores the
space of compositor edge cases that hand-written tests miss.

### Compositor-specific golden tests

Dedicated test cases for the correctness edge cases enumerated above:

- Overlapping promoted layers with different blend modes.
- Promoted element with animated opacity.
- Promoted element within a clipped group.
- Promoted element with a filter.
- Drag that causes viewport boundary crossing.
- `currentColor` change on ancestor of promoted element.

Each test renders both paths and asserts pixel identity.

## Security and Trust Boundaries

### Memory budget

Each compositor layer is a full-resolution RGBA bitmap. Budget:

| Viewport | Bytes/layer | 8 layers | 32 layers |
|----------|-------------|----------|-----------|
| 1920×1080 (1080p) | 8.3 MB | 66 MB | 266 MB |
| 2560×1440 (1440p) | 14.7 MB | 118 MB | 471 MB |
| 3840×2160 (4K) | 33.2 MB | 265 MB | 1,061 MB |

**Hard limits (compile-time constants, overridable via Bazel defines):**

```cpp
constexpr int kMaxCompositorLayers = 32;
constexpr size_t kMaxCompositorMemoryBytes = 256 * 1024 * 1024;  // 256 MB
```

When either limit is reached, `promoteEntity()` returns `false` and
the compositor falls back to full rendering for that entity. Layer
count is O(promoted entities), not O(SVG elements), so the limit is
unlikely to be hit in normal editor usage (typically 1–3 promoted
layers during drag).

### Bitmap ownership model

- `CompositorLayer` **owns** its rasterized `RendererBitmap` (via
  `std::vector<uint8_t>` inside the bitmap struct).
- During composition, the compositor passes a **non-owning**
  `RendererBitmap` reference to `drawImage()`. The reference is valid
  only for the duration of the composition call.
- `takeSnapshot()` returns a newly-allocated bitmap with its own
  `std::vector<uint8_t>`. Ownership transfers to the caller
  (`CompositorLayer`).
- No shared ownership. No reference counting on bitmaps. The
  compositor is single-threaded within a frame.

### Entity validity

`CompositorLayer` stores an `Entity` handle. The ECS registry may
invalidate entities (e.g., via `removeEntity()`). The compositor
**must** validate entity existence before accessing components:

```cpp
if (!registry_.valid(layer.entity())) {
  demoteLayer(layer);
  return;
}
```

Stale entity handles are a logic error, not a security boundary, but
the validation prevents undefined behavior from dangling entity
references. `demoteEntity()` on an already-invalid entity is a no-op.

### Trust boundary: untrusted SVG input

The compositor does not introduce new trust boundaries beyond those
already enforced by the parser and renderer. Specifically:

- Layer count is bounded by `kMaxCompositorLayers`, preventing
  unbounded memory growth from pathological promotion patterns.
- Bitmap dimensions are clamped to viewport size (the compositor
  never allocates larger-than-viewport bitmaps).
- The compositor does not interpret SVG content — it operates on
  the already-validated ECS component graph produced by the parser.

## Implementation Phases

### Phase 1: Minimum viable compositor (v1)

**Goal:** Fluid drag of ONE promoted shape with correctness guarantees.

**Prerequisites (must exist before implementation begins):**

- `Transform2d::isTranslation()` — add to `donner/base/Transform.h`.
  Returns `true` when `a() == 1 && b() == 0 && c() == 0 && d() == 1`
  (i.e., only `e` and `f` translation components are non-zero).
- `AlphaType` enum and `premultiplied` flag on `RendererBitmap` —
  required for correctness of the `takeSnapshot()` → `drawImage()` path
  (see § `RendererBitmap` adapter).
- `createOffscreenInstance()` override in at least TinySkia and Skia
  (already implemented). Geode override is optional for v1 (gated).

**Implementation steps (correctness-first order):**

1. Create `donner/svg/compositor/` package with Bazel targets.
   - *Verify:* `bazel build //donner/svg/compositor/...` succeeds.
2. Implement `CompositorLayer` with bitmap cache and dirty tracking.
   - *Verify:* Unit test: construct layer, set dirty, verify state.
3. Implement `LayerMembershipComponent` ECS component.
   - *Verify:* Unit test: attach to entity, verify retrieval.
4. Implement `CompositorController` with `promoteEntity` / `demoteEntity`.
   - *Verify:* Unit test: promote/demote, verify layer creation/destruction
     and `LayerMembershipComponent` lifecycle.
5. **Wire dual-path debug assertion harness** (before any fast path).
   - *Verify:* With assertion enabled, full-render == composited for a
     trivial scene (single rect, no promotion). This proves the
     assertion infrastructure works before fast-path code lands.
6. Implement `prepareFrame()`: consume `DirtyFlagsComponent`, mark dirty layers.
   - *Verify:* Unit test: modify entity, verify layer marked dirty.
7. Implement layer rasterization via `createOffscreenInstance()` + `drawEntityRange()`.
   - *Verify:* Integration test: promote one rect, rasterize its layer,
     pixel-diff against full render of just that rect.
8. Implement composition pass: blit layers via `drawImage()`.
   - *Verify:* Integration test: full scene with one promoted rect,
     composited output == full render (threshold=0).
9. Implement `setLayerCompositionTransform()` for translation-only drag.
   - *Verify:* Integration test: translate promoted rect by (10, 20),
     composited output == full render with translated rect.
10. Wire editor drag workflow: promote on mouse-down, update transform
    on move, demote on up.
    - *Verify:* Editor integration test: simulate drag sequence, verify
      promote/demote lifecycle and frame output.
11. Performance benchmark: 10k-element scene, single-element drag,
    measure per-frame time.
    - *Verify:* Assert < 16.67 ms on CI hardware (Apple M1 or equivalent).

### Phase 2: Mandatory promotion + multi-layer

- [ ] Auto-promote elements with opacity < 1, filter, mask, blend mode.
- [ ] Multi-layer composition ordering.
- [ ] Cross-layer clip-path handling (de-promote within clipped groups).
- [ ] Compositor golden tests for all correctness edge cases.
- [ ] Dual-path assertion enabled in CI compositor test targets.

### Phase 3: Backend optimizations (deferred)

- [ ] Implement `LayerHandle` abstraction (§ `RendererBitmap` adapter, tier 2).
  - Skia: `SkImage` COW via `surface->makeImageSnapshot()`.
  - Geode: GPU texture retention, zero-copy `drawLayer()`.
  - TinySkia: internal `blitFrom()` on premultiplied `PixmapView`.
- [ ] Skia: `SkPicture` recording per layer (gated on Phase 4 sub-layer invalidation).
- [ ] Pool `RendererBitmap` allocations — reuse across `takeSnapshot()` calls.

### Phase 4: Advanced features (deferred)

- [ ] Sub-layer dirty rectangles (tile-based invalidation within a layer).
- [ ] CSS `will-change` auto-promotion.
- [ ] Thread-pool layer rasterization.
- [ ] Mask bitmap caching.
- [ ] Filter output caching.
- [ ] Automatic layer merging for adjacent non-interacting layers.

## Open Questions

1. ~~**`RendererBitmap` → `ImageResource` adapter.**~~ — *Resolved:* see
   § `RendererBitmap` adapter and alpha model above. Two-tier approach:
   v1 adds `AlphaType` flag to `RendererBitmap` and `premultiplied`
   flag to `ImageResource`; v2 adds `LayerHandle` abstraction.

2. **Promotion cost amortization.** Promoting an element requires
   re-rasterizing the root layer without it and rasterizing the promoted
   layer. For large scenes, this is 2× the full-render cost on the first
   frame. Should the compositor pre-promote likely drag targets (e.g.,
   elements under the cursor) to hide this latency?

3. **Layer bitmap resolution.** Should layers be rasterized at viewport
   DPI or at a higher resolution for quality during zoom? Higher
   resolution wastes memory; viewport DPI re-rasterizes on zoom. v1:
   viewport DPI, re-rasterize on zoom.

4. ~~**Geode offscreen rendering.**~~ — *Resolved:* see § Geode-specific
   considerations above. Shared-device constructor with own
   `GeoEncoder` and texture pair per render target.

5. ~~**Memory budget.**~~ — *Resolved:* see § Security and Trust
   Boundaries above. Hard limits: `kMaxCompositorLayers = 32`,
   `kMaxCompositorMemoryBytes = 256 MB`. Promotion refused when
   limits reached; fallback to full rendering.

6. **Editor overlay interaction.** The editor's `OverlayRenderer` draws
   selection chrome *after* the document via direct `RendererInterface`
   calls. Should the compositor be aware of the overlay, or should the
   overlay draw on top of the composed frame? (Likely the latter — the
   overlay is not part of the SVG document and should not participate in
   layer management.)

7. **`BackgroundImage` filter detection.** How commonly is
   `BackgroundImage` used in real SVGs? If it is rare, disabling
   compositing for it is cheap. If it is common, we need a better
   strategy.

## Alternatives Considered

### A. No compositor — PGO the full-render path

**Approach:** Use profile-guided optimization to make the full-render
path fast enough for 60 fps interaction on 10k-node scenes.

**Why it doesn't work:** The full-render path is `O(N)` in scene
complexity by construction — every element is rasterized every frame.
PGO can reduce constant factors (maybe 2–3×) but cannot change the
algorithmic complexity. A 10k-node scene at ~200 ms today might reach
~80 ms with PGO — still above the 16 ms budget. The compositor reduces
interactive frames to `O(L)` where L is the number of layers (typically
2), which is fundamentally different.

PGO is still valuable *within* the compositor (reducing layer
rasterization cost) and should be pursued independently.

### B. Backend-specific compositors

**Approach:** Each backend implements its own compositor. Skia uses
`SkPicture` + `SkSurface` tile cache. Geode uses GPU render-to-texture
+ texture atlas. TinySkia uses software blit.

**Why it doesn't work well:**

1. Triples the correctness surface area. Three compositor
   implementations means three sets of edge cases, three sets of tests,
   and three chances to drift from ground truth.
2. The hard part (layer assignment, damage tracking, composition order)
   is backend-independent. Only the rasterization and blit steps differ,
   and those are already abstracted by `RendererInterface`.
3. Geode's compositor would need to track cross-layer dependencies
   (clip, mask, filter) in WGSL — a significant new shader surface with
   its own bug class.

The shared compositor with backend-internal optimizations (§ Backend
Integration) captures 90% of the per-backend benefit with 33% of the
code.

### C. Full retained-mode rendering

**Approach:** Convert the entire render pipeline to retained mode — every
element is a persistent GPU/CPU object that is updated incrementally.
No explicit compositor or layer concept; the rendering backend maintains
a scene graph internally and updates it when elements change.

**Why it's too much for v1:**

1. Retained mode requires every backend to maintain internal scene graph
   state, which is a fundamental architectural change to
   `RendererInterface` (currently stateless per frame).
2. Skia has limited retained-mode support (`SkPicture` is
   record-and-replay, not a mutable scene graph). TinySkia has none.
   Only Geode could reasonably implement retained mode.
3. The compositor approach is a natural stepping stone *toward* retained
   mode: layers are persistent objects, and the composition tree is a
   simple scene graph. If retained mode proves necessary, the compositor
   can evolve into it. Starting with retained mode would be a
   bet-the-project rewrite.

### D. Compositor at `RendererDriver` level (not ECS-aware)

**Approach:** Intercept `RendererInterface` calls from `RendererDriver`
and cache them per layer, replaying cached call sequences for clean
layers.

**Pros:** No new ECS components. Works with the existing traversal.

**Cons:** Cannot skip `RendererDriver` traversal for clean layers — the
driver must still walk the entire render tree to produce the call
sequence, even if the compositor discards most of it. The traversal
itself is `O(N)` and non-trivial (pattern/mask/marker sub-traversals).
The ECS-aware approach skips both traversal *and* rasterization for
clean layers.

This is essentially the `RendererRecorder` (Phase 3 of
[0003-renderer_interface_design](0003-renderer_interface_design.md))
repurposed as a cache. It is a reasonable v1.5 fallback if the ECS-level
approach proves too complex, but it leaves performance on the table.

## Reversibility

The compositor is designed to be fully removable without changing the
core rendering pipeline or ECS component model.

**Runtime disable:** A `DONNER_DISABLE_COMPOSITOR` flag (Bazel define
+ runtime check) bypasses the compositor entirely, routing all frames
through the existing full-render `RendererDriver::render()` path. This
flag is the primary rollback mechanism if the compositor causes
correctness or stability regressions.

**Lazy ECS attachment:** The compositor adds one ECS component
(`LayerMembershipComponent`) to promoted entities. This component is
attached lazily (only when `promoteEntity()` is called) and removed on
`demoteEntity()`. No components are modified globally — unpromoted
entities are untouched. Removing the compositor feature deletes the
`donner/svg/compositor/` package and the `LayerMembershipComponent`
definition; no other ECS components or systems change.

**No `RendererInterface` changes in v1:** The v1 tier-1 adapter adds
`AlphaType` to `RendererBitmap` (a non-breaking struct field addition
with a default value). The `RendererInterface` virtual table is
unchanged. Reverting the compositor does not require changing any
backend.

**Test coverage invariant:** The dual-path debug assertion (composited
output == full-render output) means the compositor can never silently
produce wrong pixels — any regression is caught immediately and the
fix is always "disable the compositor" until the root cause is found.

## References

- [0005-incremental_invalidation](0005-incremental_invalidation.md) — dirty-flag propagation
- [0020-editor](0020-editor.md) — editor interaction model
- [0003-renderer_interface_design](0003-renderer_interface_design.md) — `RendererInterface` / `RendererDriver`
- [0017-geode_renderer](0017-geode_renderer.md) — Geode GPU backend
- [Chromium Compositor](https://chromium.googlesource.com/chromium/src/+/HEAD/cc/) — Chrome's layer compositor (cc/)
- [WebKit Compositing](https://webkit.org/blog/12610/release-notes-for-safari-technology-preview-157/) — WebKit layer tree
- [Firefox Layers](https://searchfox.org/mozilla-central/source/gfx/layers) — Gecko layer system

## Next Steps

1. **Implement v1 (Phase 1)** — compositor skeleton, dual-path debug
   assertion, translation-only drag fast path, editor integration.
   Target: separate PR off `main`, linked from this design doc.
2. **Implement Geode `createOffscreenInstance()`** — prerequisite for
   Geode participation in compositor tests. Can be done in parallel
   with v1 (separate PR).
3. **Root-cause the premultiply roundtrip in existing
   `takeSnapshot()` → `drawImage()` paths** — this is a pre-existing
   correctness bug independent of the compositor. Fix alongside the
   tier-1 `AlphaType` adapter work.
