#include "src/svg/components/registry.h"

namespace donner {

std::string_view TypeToString(ElementType type) {
  switch (type) {
    case ElementType::SVG: return "SVG";
    case ElementType::Path: return "Path";
    case ElementType::Rect: return "Rect";
    case ElementType::Style: return "Style";
    case ElementType::Unknown: return "Unknown";
  }
}
}  // namespace donner
