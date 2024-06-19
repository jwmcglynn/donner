#include "donner/svg/properties/PaintServer.h"

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
    const PaintServer::ElementReference& ref = paint.get<PaintServer::ElementReference>();
    os << "url(" << ref.reference.href << ")";
    if (ref.fallback) {
      os << " " << *ref.fallback;
    }
  }
  os << ")";
  return os;
}

}  // namespace donner::svg
