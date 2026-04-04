#include "donner/svg/SVGTextElement.h"

#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"

namespace donner::svg {

SVGTextElement SVGTextElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);
  handle.emplace<components::TextRootComponent>();

  return SVGTextElement(handle);
}

std::vector<PathSpline> SVGTextElement::convertToPath() const {
  return computedGlyphPaths();
}

Boxd SVGTextElement::inkBoundingBox() const {
  return computedInkBounds();
}

Boxd SVGTextElement::objectBoundingBox() const {
  return computedObjectBoundingBox();
}

}  // namespace donner::svg
