#pragma once
/// @file

#include <optional>
#include <ostream>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/base/Vector2.h"
#include "donner/svg/core/CursorType.h"

namespace donner::svg {

/// One authored cursor image candidate and its optional hotspot.
struct CursorImage {
  RcString url;                     ///< URL as authored inside `url()`.
  std::optional<Vector2d> hotspot;  ///< Optional hotspot in image coordinates.

  bool operator==(const CursorImage&) const = default;
};

/// Cascaded CSS cursor value with authored URL candidates and a required keyword fallback.
struct Cursor {
  std::vector<CursorImage> images;
  CursorType fallback = CursorType::Auto;

  bool operator==(const Cursor&) const = default;
};

/// Human-readable output for diagnostics and tests.
inline std::ostream& operator<<(std::ostream& os, const Cursor& cursor) {
  for (const CursorImage& image : cursor.images) {
    os << "url(" << image.url << ")";
    if (image.hotspot) {
      os << " " << image.hotspot->x << " " << image.hotspot->y;
    }
    os << ", ";
  }
  return os << cursor.fallback;
}

}  // namespace donner::svg
