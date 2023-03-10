#pragma once
/// @file

#include <vector>

#include "src/svg/svg_geometry_element.h"

namespace donner::svg {

class SVGPolygonElement : public SVGGeometryElement {
protected:
  explicit SVGPolygonElement(EntityHandle handle) : SVGGeometryElement(handle) {}

public:
  static constexpr ElementType Type = ElementType::Polygon;
  static constexpr std::string_view Tag = "polygon";

  static SVGPolygonElement Create(SVGDocument& document);

  void setPoints(std::vector<Vector2d> value);
  const std::vector<Vector2d>& points() const;

private:
  void invalidate() const;
};

}  // namespace donner::svg
