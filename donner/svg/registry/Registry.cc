#include "donner/svg/registry/Registry.h"

#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

std::ostream& operator<<(std::ostream& os, ElementType type) {
  switch (type) {
    case ElementType::Circle: os << "Circle";
    case ElementType::ClipPath: os << "ClipPath";
    case ElementType::Defs: os << "Defs";
    case ElementType::Ellipse: os << "Ellipse";
    case ElementType::FeGaussianBlur: os << "FeGaussianBlur";
    case ElementType::Filter: os << "Filter";
    case ElementType::G: os << "G";
    case ElementType::Line: os << "Line";
    case ElementType::LinearGradient: os << "LinearGradient";
    case ElementType::Use: os << "Use";
    case ElementType::SVG: os << "SVG";
    case ElementType::Path: os << "Path";
    case ElementType::Pattern: os << "Pattern";
    case ElementType::Polygon: os << "Polygon";
    case ElementType::Polyline: os << "Polyline";
    case ElementType::RadialGradient: os << "RadialGradient";
    case ElementType::Rect: os << "Rect";
    case ElementType::Stop: os << "Stop";
    case ElementType::Style: os << "Style";
    case ElementType::Unknown: os << "Unknown";
  }

  UTILS_UNREACHABLE();
}
}  // namespace donner::svg
