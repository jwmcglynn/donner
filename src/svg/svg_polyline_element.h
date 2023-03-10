#pragma once
/// @file

#include <vector>

#include "src/svg/svg_geometry_element.h"

namespace donner::svg {

class SVGPolylineElement : public SVGGeometryElement {
protected:
  explicit SVGPolylineElement(EntityHandle handle) : SVGGeometryElement(handle) {}

public:
  static constexpr ElementType Type = ElementType::Polyline;
  static constexpr std::string_view Tag = "polyline";

  static SVGPolylineElement Create(SVGDocument& document);

  void setPoints(std::vector<Vector2d> value);
  const std::vector<Vector2d>& points() const;

private:
  void invalidate() const;
};

}  // namespace donner::svg
