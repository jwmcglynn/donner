#include "src/svg/svg_svg_element.h"

#include "src/svg/components/viewbox_component.h"
#include "src/svg/svg_document.h"

namespace donner {

SVGSVGElement SVGSVGElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGSVGElement(registry, CreateEntity(registry, Type));
}

void SVGSVGElement::setViewbox(Boxd viewbox, PreserveAspectRatio preserveAspectRatio) {
  registry_.get().emplace_or_replace<ViewboxComponent>(entity_, viewbox, preserveAspectRatio);
}

void SVGSVGElement::clearViewbox() {
  registry_.get().remove_if_exists<ViewboxComponent>(entity_);
}

std::optional<Boxd> SVGSVGElement::viewbox() const {
  if (const auto* component = registry_.get().try_get<ViewboxComponent>(entity_)) {
    return component->viewbox;
  }

  return std::nullopt;
}

std::optional<PreserveAspectRatio> SVGSVGElement::preserveAspectRatio() const {
  if (const auto* component = registry_.get().try_get<ViewboxComponent>(entity_)) {
    return component->preserveAspectRatio;
  }

  return std::nullopt;
}

}  // namespace donner