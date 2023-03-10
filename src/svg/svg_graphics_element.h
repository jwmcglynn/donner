#pragma once
/// @file

#include "src/svg/svg_element.h"

namespace donner::svg {

class SVGGraphicsElement : public SVGElement {
protected:
  explicit SVGGraphicsElement(EntityHandle handle);

public:
  Transformd transform() const;
  void setTransform(Transformd transform);

protected:
  void invalidateTransform();
  void computeTransform() const;
};

}  // namespace donner::svg
