#include "donner/svg/registry/ElementType.h"

#include "donner/base/Utils.h"

namespace donner::svg {

std::ostream& operator<<(std::ostream& os, ElementType type) {
  switch (type) {
    case ElementType::Circle: return os << "Circle";
    case ElementType::ClipPath: return os << "ClipPath";
    case ElementType::Defs: return os << "Defs";
    case ElementType::Ellipse: return os << "Ellipse";
    case ElementType::FeGaussianBlur: return os << "FeGaussianBlur";
    case ElementType::Filter: return os << "Filter";
    case ElementType::G: return os << "G";
    case ElementType::Image: return os << "Image";
    case ElementType::Line: return os << "Line";
    case ElementType::LinearGradient: return os << "LinearGradient";
    case ElementType::Mask: return os << "Mask";
    case ElementType::Use: return os << "Use";
    case ElementType::SVG: return os << "SVG";
    case ElementType::Path: return os << "Path";
    case ElementType::Pattern: return os << "Pattern";
    case ElementType::Polygon: return os << "Polygon";
    case ElementType::Polyline: return os << "Polyline";
    case ElementType::RadialGradient: return os << "RadialGradient";
    case ElementType::Rect: return os << "Rect";
    case ElementType::Stop: return os << "Stop";
    case ElementType::Style: return os << "Style";
    case ElementType::Unknown: return os << "Unknown";
  }

  UTILS_UNREACHABLE();
}
}  // namespace donner::svg
