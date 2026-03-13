# SVG Interactivity Design

## Status

**Phase:** Implementation complete (Phases 1-6)
**Dependencies:** Rendering pipeline (complete), Animation system (complete), Composited rendering (in progress)

## Overview

Donner currently provides a minimal hit-testing API (`DonnerController::findIntersecting`) that performs a brute-force O(n) reverse scan of the flat render tree. This design expands interactivity into a full-featured system supporting efficient spatial queries, DOM event dispatch, cursor management, and integration with the composited rendering pipeline.

## Current State

### What Exists

| Component | Status | Location |
|-----------|--------|----------|
| `findIntersecting(point)` | Working, O(n) | `DonnerController.cc`, `RenderingContext.cc` |
| `pointer-events` CSS property | Parsed, partially applied | `PointerEvents.h`, `PropertyRegistry.cc` |
| `PathSpline::isInside()` | Complete (winding number) | `PathSpline.cc` |
| `PathSpline::isOnPath()` | Complete (distance-based) | `PathSpline.cc` |
| AABB pre-filter | Working | `RenderingContext.cc:624` |
| `RenderingInstanceComponent` | Has drawOrder, transforms, bounds | `RenderingInstanceComponent.h` |
| Cursor CSS property | Listed but unparsed | `PropertyRegistry.cc:658` |
| Event attributes/handlers | Not implemented | N/A |

### Current Hit-Testing Algorithm

```
findIntersecting(worldPoint):
  for each entity in reverse drawOrder:
    if pointer-events == None: skip
    worldBounds = getShapeWorldBounds(entity).inflatedBy(strokeWidth)
    if !worldBounds.contains(worldPoint): skip     // AABB rejection
    if pointer-events == BoundingBox: return entity
    localPoint = entityFromWorldTransform.inverse() * worldPoint
    if fill && pathFillIntersects(localPoint): return entity
    if stroke && pathStrokeIntersects(localPoint): return entity
  return null
```

**Cost:** O(n) per query where n = number of rendering instances. For a 1,000-element SVG, every mouse move tests up to 1,000 bounding boxes. The AABB pre-filter helps but doesn't change the asymptotic cost.

## Design

### Phase 1: Spatial Index for O(log n) Hit Testing

Replace the linear scan with a spatial index that narrows candidates to elements whose bounds overlap the query point.

#### Data Structure: Flat Grid Index

For SVG documents rendered at a known canvas size, a uniform grid (cell-based spatial hash) provides the best constant factors for point queries:

```cpp
class SpatialGrid {
public:
  /// Construct a grid covering `worldBounds` with cells of `cellSize`.
  SpatialGrid(Boxd worldBounds, double cellSize);

  /// Insert an entity's world-space AABB into the grid.
  void insert(Entity entity, const Boxd& worldBounds);

  /// Remove an entity from the grid.
  void remove(Entity entity);

  /// Update an entity's position (remove + re-insert).
  void update(Entity entity, const Boxd& oldBounds, const Boxd& newBounds);

  /// Return all entities whose cells contain `point`, in reverse drawOrder.
  SmallVector<Entity, 8> query(const Vector2d& point) const;

  /// Return all entities whose cells overlap `rect`.
  SmallVector<Entity, 16> queryRect(const Boxd& rect) const;

  /// Rebuild the entire index from the current render tree.
  void rebuild(const Registry& registry);

private:
  Boxd worldBounds_;
  double cellSize_;
  int cols_, rows_;
  // Each cell stores a sorted vector of (drawOrder, entity) pairs.
  std::vector<SmallVector<std::pair<int, Entity>, 4>> cells_;
  // Reverse map: entity -> set of cell indices (for removal).
  entt::dense_map<Entity, SmallVector<int, 4>> entityCells_;
};
```

**Why a grid instead of an R-tree:**
- Point queries dominate (mouse position testing). Grid point queries are O(1) cell lookup + k candidates, vs O(log n) for R-tree.
- SVG documents have a known, bounded canvas. The grid covers this area exactly.
- Insertion/removal is O(cells-per-entity), typically 1-4 for reasonable cell sizes.
- Much simpler implementation — no tree balancing, no node splitting.
- Memory is bounded: `(canvasWidth / cellSize) * (canvasHeight / cellSize)` cells.

**Cell size heuristic:** `cellSize = max(canvasWidth, canvasHeight) / sqrt(n)` where n = number of elements. For a 1024x1024 canvas with 1,000 elements, cellSize ~ 32px, yielding a 32x32 grid (1,024 cells). Each cell averages ~1 entity.

**Grid sizing for very small documents:** If n < 16, skip the grid entirely and fall back to the existing linear scan. The overhead of building a grid for a handful of elements isn't worthwhile.

#### Integration with Render Tree

The spatial grid is rebuilt when the render tree is instantiated (same lifecycle as `RenderingInstanceComponent`):

```cpp
class RenderingContext {
  // ...
  void instantiateRenderTree(bool forceRebuild, SpatialGrid* outGrid);
  Entity findIntersecting(const Vector2d& point);
  std::vector<Entity> findAllIntersecting(const Vector2d& point);
  std::vector<Entity> findIntersectingRect(const Boxd& rect);
};
```

The grid is owned by `DonnerController` and persisted across frames. On each frame:
1. If the render tree is clean (no layout/style changes), reuse the existing grid.
2. If dirty, rebuild the grid during `instantiateRenderTree()`.
3. For incremental animation updates (composited rendering), update only the affected entities.

#### Incremental Updates for Animation

When the composited renderer identifies dirty layers, only entities within those layers need spatial index updates:

```cpp
void SpatialGrid::updateDirtyEntities(
    const Registry& registry,
    std::span<const Entity> dirtyEntities) {
  for (Entity entity : dirtyEntities) {
    auto oldBounds = entityCells_.contains(entity)
        ? reconstructBounds(entity)  // from cell membership
        : Boxd{};
    auto newBounds = ShapeSystem().getShapeWorldBounds(
        EntityHandle(registry, entity));
    if (newBounds) {
      update(entity, oldBounds, *newBounds);
    } else {
      remove(entity);
    }
  }
}
```

This makes animated scenes O(k) per frame where k = number of moving entities, not O(n) for a full rebuild.

### Phase 2: Complete `pointer-events` Implementation

The current code only handles `None` and `BoundingBox`. The full SVG2 spec defines 10 modes with visibility and paint interactions:

| Mode | Fill test | Stroke test | Requires visible | Requires painted |
|------|-----------|-------------|------------------|------------------|
| `visiblePainted` (default) | Yes, if fill != none | Yes, if stroke != none | Yes | Yes |
| `visibleFill` | Yes | No | Yes | No |
| `visibleStroke` | No | Yes | Yes | No |
| `visible` | Yes | Yes | Yes | No |
| `painted` | Yes, if fill != none | Yes, if stroke != none | No | Yes |
| `fill` | Yes | No | No | No |
| `stroke` | No | Yes | No | No |
| `all` | Yes | Yes | No | No |
| `none` | Skip | Skip | N/A | N/A |
| `bounding-box` | AABB only | AABB only | No | No |

Implementation in `findIntersecting`:

```cpp
struct HitTestConfig {
  bool testFill;
  bool testStroke;
  bool requireVisible;
  bool requirePainted;  // fill/stroke must be non-none
};

HitTestConfig configFromPointerEvents(PointerEvents pe) {
  switch (pe) {
    case PointerEvents::VisiblePainted:
      return {true, true, true, true};
    case PointerEvents::VisibleFill:
      return {true, false, true, false};
    // ... etc
  }
}
```

This replaces the current ad-hoc if/else chain with a table-driven approach.

### Phase 3: Event System

#### Design Goals

1. **Library provides dispatch, application provides I/O.** Donner does not own the event loop or window system. The application calls `document.dispatchEvent(event)` and Donner routes it to the correct element.
2. **DOM Events Level 2 model.** Capture phase (root to target), target phase, bubble phase (target to root). `stopPropagation()` and `preventDefault()` supported.
3. **No JavaScript engine.** Event handlers are C++ callbacks registered via the public API. SVG `onclick` attributes are not evaluated — they're stored as strings for applications that want to interpret them.

#### Event Types

```cpp
enum class EventType {
  // Mouse events
  Click,
  DblClick,
  MouseDown,
  MouseUp,
  MouseMove,
  MouseEnter,   // No bubble
  MouseLeave,   // No bubble
  MouseOver,    // Bubbles
  MouseOut,     // Bubbles

  // Pointer events (W3C Pointer Events)
  PointerDown,
  PointerUp,
  PointerMove,
  PointerEnter,
  PointerLeave,
  PointerOver,
  PointerOut,

  // Focus events
  FocusIn,
  FocusOut,

  // Wheel
  Wheel,
};
```

#### Event Object

```cpp
struct Event {
  EventType type;
  Vector2d clientPosition;   // In canvas coordinates
  Vector2d documentPosition; // In document coordinates (after viewBox transform)
  int button = 0;            // Mouse button (0=left, 1=middle, 2=right)
  int buttons = 0;           // Bitmask of pressed buttons
  bool ctrlKey = false;
  bool shiftKey = false;
  bool altKey = false;
  bool metaKey = false;

  // Dispatch state (set during propagation)
  Entity target = entt::null;
  Entity currentTarget = entt::null;
  enum class Phase { None, Capture, Target, Bubble } phase = Phase::None;
  bool propagationStopped = false;
  bool defaultPrevented = false;

  void stopPropagation() { propagationStopped = true; }
  void preventDefault() { defaultPrevented = true; }
};
```

#### Listener Registration

```cpp
using EventCallback = std::function<void(Event&)>;

class SVGElement {
  // ...
  /// Register an event listener. Returns a handle for removal.
  ListenerHandle addEventListener(EventType type, EventCallback callback,
                                   bool useCapture = false);

  /// Remove a previously registered listener.
  void removeEventListener(ListenerHandle handle);
};
```

Internally, listeners are stored in an ECS component:

```cpp
struct EventListenersComponent {
  struct Entry {
    EventType type;
    EventCallback callback;
    bool useCapture;
    uint32_t id;  // For ListenerHandle
  };
  SmallVector<Entry, 2> listeners;
  uint32_t nextId = 0;
};
```

#### Dispatch Algorithm

```cpp
void EventSystem::dispatch(Registry& registry, Event& event) {
  // 1. Hit test to find target (uses spatial grid).
  Entity target = findIntersecting(event.documentPosition);
  if (target == entt::null) return;
  event.target = target;

  // 2. Build ancestor path: [root, ..., parent, target].
  SmallVector<Entity, 16> path;
  buildAncestorPath(registry, target, path);

  // 3. Capture phase: root to target (exclusive).
  event.phase = Event::Phase::Capture;
  for (size_t i = 0; i < path.size() - 1 && !event.propagationStopped; ++i) {
    event.currentTarget = path[i];
    fireListeners(registry, path[i], event, /*capture=*/true);
  }

  // 4. Target phase.
  event.phase = Event::Phase::Target;
  event.currentTarget = target;
  fireListeners(registry, target, event, /*capture=*/true);
  fireListeners(registry, target, event, /*capture=*/false);

  // 5. Bubble phase: target parent to root.
  if (!event.propagationStopped && eventBubbles(event.type)) {
    event.phase = Event::Phase::Bubble;
    for (int i = static_cast<int>(path.size()) - 2;
         i >= 0 && !event.propagationStopped; --i) {
      event.currentTarget = path[i];
      fireListeners(registry, path[i], event, /*capture=*/false);
    }
  }
}
```

#### Mouse Enter/Leave Tracking

`mouseenter`/`mouseleave` require tracking which element the pointer is currently over:

```cpp
struct PointerStateComponent {
  Entity hoveredEntity = entt::null;
  Vector2d lastPosition;
};
```

On each `MouseMove`:
1. Hit test to find new hovered entity.
2. If different from `hoveredEntity`:
   - Fire `mouseleave` on old entity (no bubble).
   - Fire `mouseenter` on new entity (no bubble).
   - Fire `mouseout` on old entity (bubbles).
   - Fire `mouseover` on new entity (bubbles).
3. Update `hoveredEntity`.

### Phase 4: Cursor Management

#### Cursor Property Parsing

Promote `cursor` from unparsed to a typed property:

```cpp
enum class CursorType {
  Auto,
  Default,
  Pointer,
  Crosshair,
  Move,
  Text,
  Wait,
  Help,
  NotAllowed,
  Grab,
  Grabbing,
  // Resize cursors
  NResize, EResize, SResize, WResize,
  NEResize, NWResize, SEResize, SWResize,
  ColResize, RowResize,
  ZoomIn, ZoomOut,
  None,
};
```

#### Cursor Query API

Applications need to know what cursor to display:

```cpp
class DonnerController {
  // ...
  /// Returns the cursor type for the element at `point`.
  /// Returns CursorType::Auto if no element or no cursor set.
  CursorType getCursorAt(const Vector2d& point);
};
```

This uses the spatial grid to find the element, walks up the ancestor chain (cursor is inherited), and returns the resolved cursor. The application maps `CursorType` to platform cursor (e.g., GLFW, SDL).

### Phase 5: Public API Expansion

Expand `DonnerController` and `SVGDocument` for editor/interactive use cases:

```cpp
class DonnerController {
public:
  explicit DonnerController(SVGDocument document);

  // --- Existing ---
  std::optional<SVGGeometryElement> findIntersecting(const Vector2d& point);

  // --- New: Spatial Queries ---

  /// Find all elements at `point`, ordered front-to-back.
  std::vector<SVGElement> findAllIntersecting(const Vector2d& point);

  /// Find all elements intersecting `rect`, ordered front-to-back.
  std::vector<SVGElement> findIntersectingRect(const Boxd& rect);

  /// Get the world-space bounding box of an element (including stroke).
  std::optional<Boxd> getWorldBounds(SVGElement element);

  // --- New: Event Dispatch ---

  /// Dispatch a mouse/pointer event. Returns the target element.
  std::optional<SVGElement> dispatchEvent(Event& event);

  /// Get the cursor that should be displayed at `point`.
  CursorType getCursorAt(const Vector2d& point);

  // --- New: Hover State ---

  /// Update hover tracking for a mouse move. Fires enter/leave events.
  /// Call this on every mouse move for correct event semantics.
  void updateHover(const Vector2d& point);

  /// Get the currently hovered element.
  std::optional<SVGElement> hoveredElement() const;
};
```

### Phase 6: Integration with Composited Rendering

The composited renderer already tracks dirty layers. Tie this into the spatial index:

1. **Layer-aware index updates:** When a compositing layer is marked dirty (animation changed an element's transform or geometry), only re-index entities within that layer.
2. **Cached hit paths:** For elements that haven't changed since the last frame, skip re-computing `entityFromWorldTransform` during hit testing.
3. **Hit test on composited output:** For elements with filters or masks, hit testing on the source geometry may not match the visual output. Phase 6 adds an option to hit-test against the composited layer's alpha channel as a fallback.

## Architecture

```
Application (GLFW/SDL/Qt)
  │
  │  Raw mouse/keyboard events
  ▼
DonnerController
  │
  ├── updateHover(point)  ──→  SpatialGrid::query(point)
  │                              │
  │                              ▼
  │                         Candidate entities (AABB match)
  │                              │
  │                              ▼
  │                         Precise hit test (pointer-events rules)
  │                              │
  │                              ▼
  │                         Target entity
  │
  ├── dispatchEvent(event) ──→  EventSystem::dispatch()
  │                              │
  │                              ├── Capture phase (root → target)
  │                              ├── Target phase
  │                              └── Bubble phase (target → root)
  │
  └── getCursorAt(point)   ──→  SpatialGrid::query(point)
                                  │
                                  ▼
                                 Resolve cursor from style cascade
```

## File Plan

| File | Phase | Description |
|------|-------|-------------|
| `donner/svg/components/SpatialGrid.h` | 1 | Grid spatial index |
| `donner/svg/components/SpatialGrid.cc` | 1 | Grid implementation |
| `donner/svg/components/tests/SpatialGrid_tests.cc` | 1 | Unit tests |
| `donner/svg/renderer/RenderingContext.cc` | 1, 2 | Integrate grid, complete pointer-events |
| `donner/svg/DonnerController.h` | 1, 3, 5 | Expanded public API |
| `donner/svg/DonnerController.cc` | 1, 3, 5 | Implementation |
| `donner/svg/core/Event.h` | 3 | Event types and Event struct |
| `donner/svg/components/EventListenersComponent.h` | 3 | ECS component for listeners |
| `donner/svg/components/EventSystem.h` | 3 | Event dispatch system |
| `donner/svg/components/EventSystem.cc` | 3 | Dispatch implementation |
| `donner/svg/core/CursorType.h` | 4 | Cursor enum |
| `donner/svg/properties/PropertyRegistry.cc` | 4 | Parse cursor property |

## Implementation Order

### Phase 1: Spatial Grid (prerequisite for everything else)
1. Implement `SpatialGrid` with insert/remove/query/queryRect
2. Add rebuild from `RenderingInstanceComponent` view
3. Integrate into `RenderingContext::findIntersecting` — use grid for candidate selection, existing precise tests for confirmation
4. Add `findAllIntersecting` and `findIntersectingRect` to `DonnerController`
5. Unit tests: grid correctness, edge cases (elements spanning many cells, empty grid, single-element)
6. Benchmark: compare O(n) scan vs grid query for 100, 1K, 10K element SVGs

### Phase 2: Complete pointer-events
1. Implement table-driven `HitTestConfig` from `PointerEvents` enum
2. Add visibility checking to hit test (check `visible` on `RenderingInstanceComponent`)
3. Handle "painted" vs "geometric" distinction (fill/stroke none vs present)
4. Test all 10 pointer-events modes

### Phase 3: Event System
1. Define `Event` struct and `EventType` enum
2. Implement `EventListenersComponent` and `addEventListener`/`removeEventListener`
3. Implement `EventSystem::dispatch` with capture/target/bubble phases
4. Add `PointerStateComponent` for hover tracking
5. Implement `mouseenter`/`mouseleave`/`mouseover`/`mouseout` semantics
6. Wire into `DonnerController::dispatchEvent` and `updateHover`

### Phase 4: Cursor Management
1. Define `CursorType` enum
2. Parse `cursor` CSS property in `PropertyRegistry`
3. Implement `DonnerController::getCursorAt` using spatial grid + style cascade
4. Update viewer example to set platform cursor

### Phase 5: Public API Polish
1. Add remaining query methods to `DonnerController`
2. Add `getWorldBounds` for editor selection rectangles
3. Documentation and examples

### Phase 6: Composited Rendering Integration
1. Incremental grid updates from dirty layer tracking
2. Alpha-channel hit testing for filtered elements
3. Performance optimization for animated scenes

## Performance Targets

| Operation | Current | Target (Phase 1) | Target (Phase 6) |
|-----------|---------|-------------------|-------------------|
| Point hit test (1K elements) | ~50us | ~5us | ~3us |
| Point hit test (10K elements) | ~500us | ~8us | ~5us |
| Rect query (1K elements) | N/A | ~20us | ~15us |
| Index rebuild (1K elements) | N/A | ~200us | N/A |
| Incremental update (1 entity) | N/A | N/A | ~1us |
| Event dispatch (3-level path) | N/A | ~2us | ~2us |

These targets assume the spatial grid reduces candidates to <10 per query, and precise hit testing costs ~0.5us per candidate (transform + winding number).

## Risks and Alternatives

**Grid vs R-tree:** The grid assumes roughly uniform element distribution. If an SVG has extreme clustering (e.g., 10,000 elements in one corner), some cells will be overloaded. Mitigation: cap cell occupancy and fall back to linear scan for overloaded cells. If this becomes a real problem, swap the grid for an R-tree without changing the public API.

**Floating-point bounds:** World-space AABB computation for transformed elements can be imprecise. Use conservative (inflated) bounds in the grid to avoid false negatives.

**Text hit testing:** Text elements use glyph outlines for rendering but don't currently expose them through `PathSpline`. Phase 1 will use bounding-box hit testing for text; precise glyph-level hit testing is deferred.

**Event ordering guarantees:** The SVG spec requires events to fire in document order during the bubble phase. Since we walk the DOM tree (not the render tree) for bubbling, this is naturally correct. But `findIntersecting` uses render-tree order (drawOrder) for z-ordering. These two orderings can differ when `z-index` or isolation is involved — document the distinction.
