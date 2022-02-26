#pragma once

#include "src/svg/core/gradient.h"
#include "src/svg/svg_element.h"

namespace donner::svg {

class SVGGradientElement : public SVGElement {
protected:
  // SVGGradientElement must be constructed from a derived class.
  explicit SVGGradientElement(EntityHandle handle);

public:
  std::optional<RcString> href() const;
  GradientUnits gradientUnits() const;
  GradientSpreadMethod spreadMethod() const;

  void setHref(std::optional<RcString> value);
  void setGradientUnits(GradientUnits value);
  void setSpreadMethod(GradientSpreadMethod value);

protected:
  void invalidate() const;
};

}  // namespace donner::svg
