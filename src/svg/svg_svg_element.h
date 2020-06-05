#pragma once

#include <optional>

#include "src/base/box.h"
#include "src/base/length.h"
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

  void setX(Lengthd value);
  void setY(Lengthd value);
  void setWidth(Lengthd value);
  void setHeight(Lengthd value);
  void setViewbox(Boxd viewbox);
  void setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio);
  void clearViewbox();

  Lengthd x() const;
  Lengthd y() const;
  Lengthd width() const;
  Lengthd height() const;
  std::optional<Boxd> viewbox() const;
  std::optional<PreserveAspectRatio> preserveAspectRatio() const;
};

}  // namespace donner