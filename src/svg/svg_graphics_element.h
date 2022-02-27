#pragma once

#include "src/svg/svg_element.h"

namespace donner::svg {

class SVGGraphicsElement : public SVGElement {
protected:
  explicit SVGGraphicsElement(EntityHandle handle);
};

}  // namespace donner::svg
