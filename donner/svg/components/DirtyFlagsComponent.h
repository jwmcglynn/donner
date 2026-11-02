#pragma once
/// @file

#include <cstdint>

namespace donner::svg::components {

/**
 * Tracks which computed properties are stale and need recomputation after a DOM mutation.
 *
 * Attached to entities that have been mutated since the last render. Systems check these flags
 * during `createComputedComponents()` to skip clean entities and only recompute what changed.
 *
 * @ingroup ecs_components
 */
struct DirtyFlagsComponent {
  /// Individual dirty flags for each computation stage.
  enum Flags : uint16_t {
    None = 0,
    Style = 1 << 0,           //!< ComputedStyleComponent needs recomputation.
    Layout = 1 << 1,          //!< ComputedSizedElementComponent / viewBox needs recomputation.
    Transform = 1 << 2,       //!< ComputedLocalTransformComponent needs recomputation.
    WorldTransform = 1 << 3,  //!< ComputedAbsoluteTransformComponent needs recomputation.
    Shape = 1 << 4,           //!< ComputedPathComponent needs recomputation.
    Paint = 1 << 5,           //!< ResolvedPaintServer (fill/stroke) needs re-resolution.
    Filter = 1 << 6,          //!< Filter effect chain needs re-resolution.
    RenderInstance = 1 << 7,  //!< RenderingInstanceComponent needs update.
    ShadowTree = 1 << 8,      //!< Shadow tree needs re-instantiation.
    TextGeometry = 1 << 9,    //!< ComputedTextGeometryComponent needs recomputation.

    /// Compound flag: style change that may affect paint, filter, and render instance.
    StyleCascade = Style | Paint | Filter | RenderInstance,
    /// Compound flag: layout change that affects transforms and render instance.
    LayoutCascade = Layout | Transform | WorldTransform | RenderInstance,
    /// All flags set.
    All = 0xFFFF,
  };

  uint16_t flags = Flags::None;  //!< Bitfield of dirty flags.

  /// Mark additional flags as dirty.
  void mark(uint16_t f) { flags |= f; }

  /// Test whether any of the given flags are set.
  bool test(uint16_t f) const { return (flags & f) != 0; }

  /// Clear specific flags.
  void clear(uint16_t f) { flags &= ~f; }

  /// Clear all flags.
  void clearAll() { flags = Flags::None; }
};

/**
 * Global invalidation state, stored in the registry context via `Registry::ctx()`.
 *
 * Tracks whether a full render tree rebuild is required (e.g., after `setTime()` or
 * `setCanvasSize()`), separate from per-entity dirty flags.
 */
struct RenderTreeState {
  /// True if the render tree needs a full rebuild (structure changed, canvas resized, etc.).
  bool needsFullRebuild = true;

  /// True if styles must be recomputed for the whole tree because selector dependencies may be
  /// non-local (for example class/id/attribute changes or tree mutations).
  bool needsFullStyleRecompute = true;

  /// True if the render tree has been built at least once.
  bool hasBeenBuilt = false;

  /// True if only the rendering instances must be rebuilt from the current computed components,
  /// for example after the context paint an external `<use>` supplies changes.
  bool needsInstanceRebuild = false;

  /**
   * When false, every dirty frame takes the full rebuild path (shadow-tree teardown and whole
   * document recomputation) even if its dirty set qualifies for a retained update. Lets tests
   * and diagnostics compare a retained update against the full rebuild of the same state.
   */
  bool retainedUpdatesEnabled = true;

  /// Observed render-tree preparation work, for tests and diagnostics.
  struct Counters {
    uint64_t fullRebuilds = 0;         //!< Prepares that tore down and recomputed everything.
    uint64_t retainedUpdates = 0;      //!< Prepares that kept the computed components.
    uint64_t shadowTreeTeardowns = 0;  //!< Shadow-tree hosts torn down by full rebuilds.
    /// External `<use>` instances re-attached in place, after a retarget or a geometry change.
    uint64_t retargetedUses = 0;
  };

  /// Preparation counters for this document.
  Counters counters;
};

}  // namespace donner::svg::components
