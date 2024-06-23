#include "donner/svg/registry/Registry.h"

#include "donner/base/Utils.h"

namespace donner::svg {

std::string_view TypeToString(ElementType type) {
  switch (type) {
    case ElementType::Circle: return "Circle";
    case ElementType::ClipPath: return "ClipPath";
    case ElementType::Defs: return "Defs";
    case ElementType::Ellipse: return "Ellipse";
    case ElementType::FeGaussianBlur: return "FeGaussianBlur";
    case ElementType::Filter: return "Filter";
    case ElementType::G: return "G";
    case ElementType::Line: return "Line";
    case ElementType::LinearGradient: return "LinearGradient";
    case ElementType::Use: return "Use";
    case ElementType::SVG: return "SVG";
    case ElementType::Path: return "Path";
    case ElementType::Pattern: return "Pattern";
    case ElementType::Polygon: return "Polygon";
    case ElementType::Polyline: return "Polyline";
    case ElementType::RadialGradient: return "RadialGradient";
    case ElementType::Rect: return "Rect";
    case ElementType::Stop: return "Stop";
    case ElementType::Style: return "Style";
    case ElementType::Unknown: return "Unknown";
  }

  UTILS_UNREACHABLE();
}
}  // namespace donner::svg
