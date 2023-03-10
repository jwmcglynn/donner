#pragma once
/// @file

#include "src/svg/core/path_spline.h"
#include "src/svg/svg_geometry_element.h"

namespace donner::svg {

class SVGPathElement : public SVGGeometryElement {
protected:
  explicit SVGPathElement(EntityHandle handle) : SVGGeometryElement(handle) {}

public:
  static constexpr ElementType Type = ElementType::Path;
  static constexpr std::string_view Tag = "path";

  static SVGPathElement Create(SVGDocument& document);

  RcString d() const;
  void setD(RcString d);

  std::optional<PathSpline> computedSpline() const;

private:
  void invalidate() const;
  void compute() const;
};

}  // namespace donner::svg
