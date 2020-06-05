#pragma once

#include <optional>

#include "src/base/box.h"
#include "src/svg/core/preserve_aspect_ratio.h"
#include "src/svg/svg_element.h"

namespace donner {

class SVGSVGElement : public SVGElement {
  friend class SVGDocument;

protected:
  SVGSVGElement(Registry& registry, Entity entity) : SVGElement(registry, entity) {}

public:
  static constexpr ElementType Type = ElementType::SVG;
  static constexpr std::string_view Tag = "svg";

  static SVGSVGElement Create(SVGDocument& document);

  void setViewbox(Boxd viewbox, PreserveAspectRatio preserveAspectRatio = PreserveAspectRatio());
  void clearViewbox();

  std::optional<Boxd> viewbox() const;
  std::optional<PreserveAspectRatio> preserveAspectRatio() const;
};

}  // namespace donner