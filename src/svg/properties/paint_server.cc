#include "src/svg/properties/paint_server.h"

namespace donner::svg {

bool PaintServer::operator==(const PaintServer& other) const {
  return value == other.value;
}

std::ostream& operator<<(std::ostream& os, const PaintServer& paint) {
  os << "PaintServer(";
  if (paint.is<PaintServer::None>()) {
    os << "none";
  } else if (paint.is<PaintServer::ContextFill>()) {
    os << "context-fill";
  } else if (paint.is<PaintServer::ContextStroke>()) {
    os << "context-stroke";
  } else if (paint.is<PaintServer::Solid>()) {
    os << "solid " << paint.get<PaintServer::Solid>().color;
  } else {
    const PaintServer::Reference& reference = paint.get<PaintServer::Reference>();
    os << "url(" << reference.url << ")";
    if (reference.fallback) {
      os << " " << *reference.fallback;
    }
  }
  os << ")";
  return os;
}

}  // namespace donner::svg