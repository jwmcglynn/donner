#pragma once
/// @file

#include <cstdint>

namespace donner::editor {

/// How the editor's background renderer uses the compositor, selected via the
/// View menu's "Composited Rendering" submenu. Every mode produces identical
/// pixels; the modes trade retained caching (memory, cache-management cost)
/// against per-frame re-render cost.
enum class CompositedRenderingMode : std::uint8_t {
  /// No `CompositorController` is constructed. Every frame renders the full
  /// document directly through `RendererDriver::drawInterruptibly`. The
  /// baseline for measuring what compositing buys on a given document.
  Off,
  /// Compositor runs, but only SVG filters (the expensive re-render case) get
  /// cached isolated layers. Opacity groups, blend modes, and masks render
  /// inline, and selection/drag promotion, animation promotion, and complexity
  /// bucketing are disabled.
  FilterOnly,
  /// Full compositing: mandatory hints for all isolation signals plus
  /// selection/drag promotion, animation promotion, and complexity bucketing.
  On,
};

}  // namespace donner::editor
