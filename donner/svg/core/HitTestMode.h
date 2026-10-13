#pragma once
/// @file

#include <ostream>

namespace donner::svg {

/// Select whether a hit query follows SVG interaction rules or targets editable artwork.
enum class HitTestMode {
  SvgPointerEvents,  ///< Honor the element's computed pointer-events property.
  EditorSelection,   ///< Test visible painted geometry regardless of pointer-events.
};

/// Output stream operator for HitTestMode.
inline std::ostream& operator<<(std::ostream& os, HitTestMode mode) {
  switch (mode) {
    case HitTestMode::SvgPointerEvents: return os << "SvgPointerEvents";
    case HitTestMode::EditorSelection: return os << "EditorSelection";
  }
  return os << "Unknown";
}

}  // namespace donner::svg
