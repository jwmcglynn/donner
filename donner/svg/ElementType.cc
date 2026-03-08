#include "donner/svg/ElementType.h"

#include "donner/base/Utils.h"

namespace donner::svg {

std::ostream& operator<<(std::ostream& os, ElementType type) {
  switch (type) {
    case ElementType::Circle: return os << "Circle";
    case ElementType::ClipPath: return os << "ClipPath";
    case ElementType::Defs: return os << "Defs";
    case ElementType::Ellipse: return os << "Ellipse";
    case ElementType::FeBlend: return os << "FeBlend";
    case ElementType::FeColorMatrix: return os << "FeColorMatrix";
    case ElementType::FeComponentTransfer: return os << "FeComponentTransfer";
    case ElementType::FeComposite: return os << "FeComposite";
    case ElementType::FeConvolveMatrix: return os << "FeConvolveMatrix";
    case ElementType::FeDisplacementMap: return os << "FeDisplacementMap";
    case ElementType::FeDropShadow: return os << "FeDropShadow";
    case ElementType::FeFlood: return os << "FeFlood";
    case ElementType::FeFuncA: return os << "FeFuncA";
    case ElementType::FeFuncB: return os << "FeFuncB";
    case ElementType::FeFuncG: return os << "FeFuncG";
    case ElementType::FeFuncR: return os << "FeFuncR";
    case ElementType::FeGaussianBlur: return os << "FeGaussianBlur";
    case ElementType::FeMerge: return os << "FeMerge";
    case ElementType::FeMergeNode: return os << "FeMergeNode";
    case ElementType::FeMorphology: return os << "FeMorphology";
    case ElementType::FeOffset: return os << "FeOffset";
    case ElementType::FeTile: return os << "FeTile";
    case ElementType::FeTurbulence: return os << "FeTurbulence";
    case ElementType::Filter: return os << "Filter";
    case ElementType::G: return os << "G";
    case ElementType::Image: return os << "Image";
    case ElementType::Line: return os << "Line";
    case ElementType::LinearGradient: return os << "LinearGradient";
    case ElementType::Marker: return os << "Marker";
    case ElementType::Mask: return os << "Mask";
    case ElementType::Path: return os << "Path";
    case ElementType::Pattern: return os << "Pattern";
    case ElementType::Polygon: return os << "Polygon";
    case ElementType::Polyline: return os << "Polyline";
    case ElementType::RadialGradient: return os << "RadialGradient";
    case ElementType::Rect: return os << "Rect";
    case ElementType::Stop: return os << "Stop";
    case ElementType::Style: return os << "Style";
    case ElementType::SVG: return os << "SVG";
    case ElementType::Symbol: return os << "Symbol";
    case ElementType::Text: return os << "Text";
    case ElementType::TSpan: return os << "TSpan";
    case ElementType::Unknown: return os << "Unknown";
    case ElementType::Use: return os << "Use";
  }

  UTILS_UNREACHABLE();
}
}  // namespace donner::svg
