#pragma once

#include "src/base/rc_string.h"

namespace donner {

class SVGDocument;

struct DocumentContext {
  DocumentContext(SVGDocument& document) : document(document) {}

  SVGDocument& document;
};

}  // namespace donner
