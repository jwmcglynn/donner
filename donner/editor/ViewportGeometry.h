#pragma once
/// @file

#include <optional>

#include "donner/base/Box.h"
#include "donner/base/Vector2.h"

namespace donner::editor {

/**
 * Computed layout for the Drawing window image, plus helpers for converting
 * between screen coordinates and document coordinates.
 */
struct DrawingViewportLayout {
  Vector2d imageOrigin;
  Vector2d imageSize;
  Box2d documentViewBox;

  [[nodiscard]] bool hasImage() const;
  [[nodiscard]] bool containsScreenPoint(const Vector2d& screenPoint) const;
  [[nodiscard]] std::optional<Vector2d> screenToDocument(const Vector2d& screenPoint) const;
  [[nodiscard]] std::optional<Vector2d> documentToScreen(const Vector2d& documentPoint) const;
};

/**
 * Computes the visible image placement in the Drawing window.
 *
 * @param contentOrigin Top-left screen position of the available drawing area.
 * @param availableRegionSize Size of the available drawing area in screen coordinates.
 * @param imageSize Rendered image size in screen coordinates.
 * @param panOffset Pan offset in screen coordinates.
 * @param documentViewBox Visible document-space viewBox.
 * @return Layout and coordinate-mapping helpers for the viewport.
 */
[[nodiscard]] DrawingViewportLayout ComputeDrawingViewportLayout(
    const Vector2d& contentOrigin, const Vector2d& availableRegionSize, const Vector2d& imageSize,
    const Vector2d& panOffset, const Box2d& documentViewBox);

}  // namespace donner::editor
