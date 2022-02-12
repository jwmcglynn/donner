#include "src/svg/components/rect_component.h"

#include "src/svg/parser/path_parser.h"

namespace donner {

void RectComponent::computePath(ComputedPathComponent& component, const Boxd& viewbox,
                                const FontMetrics& fontMetrics) {
  const Vector2d pos(x.toPixels(viewbox, fontMetrics), y.toPixels(viewbox, fontMetrics));
  const Vector2d size(width.toPixels(viewbox, fontMetrics), height.toPixels(viewbox, fontMetrics));

  component.setSpline(PathSpline::Builder()
                          .moveTo(pos)
                          .lineTo(pos + Vector2d(size.x, 0))
                          .lineTo(pos + size)
                          .lineTo(pos + Vector2d(0, size.y))
                          .closePath()
                          .build());
}

template <>
ParseResult<bool> svg::ParsePresentationAttribute<ElementType::Rect>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // TODO
  return false;
}

}  // namespace donner
