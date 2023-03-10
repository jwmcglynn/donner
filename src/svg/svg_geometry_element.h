#pragma once
/// @file

#include "src/svg/svg_graphics_element.h"

namespace donner::svg {

class SVGGeometryElement : public SVGGraphicsElement {
protected:
  // SVGGeometryElement must be constructed from a derived class.
  explicit SVGGeometryElement(EntityHandle handle) : SVGGraphicsElement(handle) {}

public:
  std::optional<double> pathLength() const;
  void setPathLength(std::optional<double> value);
};

}  // namespace donner::svg
