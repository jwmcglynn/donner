#pragma once

#include "src/svg/core/gradient.h"
#include "src/svg/svg_element.h"

namespace donner::svg {

/**
 * @brief Base class for SVG gradient elements, such as \ref SVGLinearGradientElement and \ref
 * SVGRadialGradientElement.
 */
class SVGGradientElement : public SVGElement {
protected:
  /**
   * Constructor for SVGGradientElement, which must be constructed from a derived class.
   *
   * @param handle The handle to the underlying entity.
   */
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
