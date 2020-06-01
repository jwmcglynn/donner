#pragma once

#include "src/svg/components/registry.h"

namespace donner {

class SVGDocument {
public:
  SVGDocument();

  Registry& registry() { return registry_; }

  Registry registry_;
};

}  // namespace donner