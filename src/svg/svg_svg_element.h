#pragma once

#include <optional>

#include "src/base/box.h"
#include "src/base/length.h"
#include "src/svg/core/preserve_aspect_ratio.h"
#include "src/svg/svg_element.h"

namespace donner::svg {

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
  void setWidth(std::optional<Lengthd> value);
  void setHeight(std::optional<Lengthd> value);
  void setViewbox(std::optional<Boxd> viewbox);
  void setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio);

  Lengthd x() const;
  Lengthd y() const;
  std::optional<Lengthd> width() const;
  std::optional<Lengthd> height() const;
  std::optional<Boxd> viewbox() const;
  std::optional<PreserveAspectRatio> preserveAspectRatio() const;
};

}  // namespace donner::svg
