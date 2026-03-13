#include "donner/svg/properties/PresentationAttributeParsing.h"

#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/paint/StopComponent.h"
#include "donner/svg/components/shape/CircleComponent.h"
#include "donner/svg/components/shape/EllipseComponent.h"
#include "donner/svg/components/shape/RectComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"

namespace donner::svg::parser {

ParseResult<bool> ParsePresentationAttribute(ElementType type, EntityHandle handle,
                                             std::string_view name,
                                             const PropertyParseFnParams& params) {
  switch (type) {
    // Non-trivial: delegate to component helpers.
    case ElementType::FeFlood:
      return components::ParseFeFloodPresentationAttribute(handle, name, params);
    case ElementType::FeDropShadow:
      return components::ParseFeDropShadowPresentationAttribute(handle, name, params);
    case ElementType::FeDiffuseLighting:
      return components::ParseFeDiffuseLightingPresentationAttribute(handle, name, params);
    case ElementType::FeSpecularLighting:
      return components::ParseFeSpecularLightingPresentationAttribute(handle, name, params);

    case ElementType::SVG:
    case ElementType::Use:
    case ElementType::Image:
      return components::ParseSizedElementPresentationAttribute(handle, name, params);

    case ElementType::Rect:
      return components::ParseRectPresentationAttribute(handle, name, params);
    case ElementType::Circle:
      return components::ParseCirclePresentationAttribute(handle, name, params);
    case ElementType::Ellipse:
      return components::ParseEllipsePresentationAttribute(handle, name, params);
    case ElementType::Path:
      return components::ParsePathPresentationAttribute(handle, name, params);

    case ElementType::Stop:
      return components::ParseStopPresentationAttribute(handle, name, params);

    // All other elements have no element-specific presentation attributes.
    default:
      return false;
  }
}

}  // namespace donner::svg::parser
