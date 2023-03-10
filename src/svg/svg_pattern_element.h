#pragma once
/// @file

#include <optional>

#include "src/base/length.h"
#include "src/svg/core/pattern.h"
#include "src/svg/core/preserve_aspect_ratio.h"
#include "src/svg/svg_element.h"

namespace donner::svg {

class SVGPatternElement : public SVGElement {
protected:
  explicit SVGPatternElement(EntityHandle handle) : SVGElement(handle) {}

public:
  static constexpr ElementType Type = ElementType::Pattern;
  static constexpr std::string_view Tag = "pattern";

  static SVGPatternElement Create(SVGDocument& document);

  std::optional<Boxd> viewbox() const;
  PreserveAspectRatio preserveAspectRatio() const;
  Lengthd x() const;
  Lengthd y() const;
  std::optional<Lengthd> width() const;
  std::optional<Lengthd> height() const;
  PatternUnits patternUnits() const;
  PatternContentUnits patternContentUnits() const;
  Transformd patternTransform() const;
  std::optional<RcString> href() const;

  void setViewbox(std::optional<Boxd> viewbox);
  void setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio);
  void setX(Lengthd value);
  void setY(Lengthd value);
  void setWidth(std::optional<Lengthd> value);
  void setHeight(std::optional<Lengthd> value);
  void setPatternUnits(PatternUnits value);
  void setPatternContentUnits(PatternContentUnits value);
  void setPatternTransform(Transformd transform);
  void setHref(std::optional<RcString> value);

protected:
  void invalidateTransform();
  void computeTransform() const;
};

}  // namespace donner::svg
