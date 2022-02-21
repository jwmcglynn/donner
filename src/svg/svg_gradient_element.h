#pragma once

#include "src/svg/core/gradient.h"
#include "src/svg/svg_element.h"

namespace donner::svg {

class SVGGradientElement : public SVGElement {
protected:
  // SVGGradientElement must be constructed from a derived class.
  explicit SVGGradientElement(EntityHandle handle);

public:
  GradientUnits gradientUnits() const;
  void setGradientUnits(GradientUnits value);

  Transformd gradientTransform() const;
  void setGradientTransform(Transformd value);

  GradientSpreadMethod spreadMethod() const;
  void setSpreadMethod(GradientSpreadMethod value);

protected:
  void invalidate() const;
};

}  // namespace donner::svg
