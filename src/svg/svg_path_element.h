#pragma once

#include "src/svg/core/path_spline.h"
#include "src/svg/svg_element.h"

namespace donner::svg {

class SVGPathElement : public SVGGraphicsElement {
protected:
  explicit SVGPathElement(EntityHandle handle) : SVGGraphicsElement(handle) {}

public:
  static constexpr ElementType Type = ElementType::Path;
  static constexpr std::string_view Tag = "path";

  static SVGPathElement Create(SVGDocument& document);

  RcString d() const;
  void setD(RcString d);

  std::optional<double> pathLength() const;
  void setPathLength(std::optional<double> value);

  std::optional<PathSpline> computedSpline() const;

private:
  void invalidate() const;
  void compute() const;
};

}  // namespace donner::svg
