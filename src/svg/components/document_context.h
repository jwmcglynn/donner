#pragma once

#include "src/base/rc_string.h"
#include "src/base/vector2.h"

namespace donner {

class SVGDocument;

struct DocumentContext {
  DocumentContext(SVGDocument& document) : document(document) {}

  SVGDocument& document;
  std::optional<Vector2d> defaultSize;
};

}  // namespace donner
