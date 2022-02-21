#pragma once

#include "src/base/length.h"
#include "src/svg/svg_element.h"

namespace donner::svg {

class SVGStopElement : public SVGElement {
protected:
  explicit SVGStopElement(EntityHandle handle) : SVGElement(handle) {}

public:
  static constexpr ElementType Type = ElementType::Stop;
  static constexpr std::string_view Tag = "stop";

  static SVGStopElement Create(SVGDocument& document);

  void setOffset(float value);
  void setStopColor(css::Color value);
  void setStopOpacity(double value);

  float offset() const;
  css::Color stopColor() const;
  double stopOpacity() const;

  // offset is not a presentation property, so it is not different when computed.
  css::Color computedStopColor() const;
  double computedStopOpacity() const;

private:
  void invalidate() const;
};

}  // namespace donner::svg
