#pragma once
/// @file
///
/// **`SelectionOverlay`** — per-frame chrome data the backend ships to the
/// host for drawing. The host has no document and no element identity; it
/// receives this struct with every `Frame` and hands it to
/// `OverlayRenderer` to paint selection rectangles, handle dots, and the
/// marquee during drag-select.
///
/// See docs/design_docs/0023-editor_sandbox.md §S8 for the wire encoding.

#include <cstdint>
#include <optional>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"

namespace donner::editor {

/// One selected element's chrome payload in document space.
struct OverlaySelection {
  /// Axis-aligned world-space bbox of the element. The host applies its
  /// viewport transform before drawing.
  Box2d worldBBox{};

  /// When an element is rotated / skewed, the bbox alone under-sells its
  /// true footprint. `hasWorldTransform` means the host should draw the
  /// selection rectangle using `worldTransform` applied to the element's
  /// *local* bbox rather than using `worldBBox`.
  bool hasWorldTransform = false;
  Transform2d worldTransform;

  /// Bitmask of resize / rotate handles the backend wants rendered. The
  /// host maps bits to positions — top-left, top, top-right, etc. 0 means
  /// "no handles, just the outline."
  uint32_t handleMask = 0;
};

/// Aggregate chrome payload. The full struct rides inside every `Frame`
/// the backend sends.
struct SelectionOverlay {
  std::vector<OverlaySelection> selections;

  /// Drag-select marquee rectangle while a drag is in flight. Document
  /// space. `nullopt` when no drag is happening.
  std::optional<Box2d> marquee;

  /// Rectangle highlighting the element under the pointer when no drag
  /// is in flight — a hover cue. Document space. `nullopt` when the
  /// pointer isn't over a geometry element.
  std::optional<Box2d> hoverRect;
};

}  // namespace donner::editor
