#pragma once
/// @file

#include "src/base/rc_string.h"
#include "src/svg/svg_element.h"

namespace donner::svg {

class SVGStyleElement : public SVGElement {
protected:
  explicit SVGStyleElement(EntityHandle handle) : SVGElement(handle) {}

public:
  static constexpr ElementType Type = ElementType::Style;
  static constexpr XMLQualifiedNameRef Tag{"style"};

  static SVGStyleElement Create(SVGDocument& document);

  void setType(RcString type);
  void setContents(std::string_view style);

  bool isCssType() const;
};

}  // namespace donner::svg
