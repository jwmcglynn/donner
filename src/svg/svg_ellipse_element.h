#pragma once

#include <optional>

#include "src/base/length.h"
#include "src/svg/svg_element.h"

namespace donner::svg {

class SVGEllipseElement : public SVGGraphicsElement {
protected:
  explicit SVGEllipseElement(EntityHandle handle) : SVGGraphicsElement(handle) {}

public:
  static constexpr ElementType Type = ElementType::Ellipse;
  static constexpr std::string_view Tag = "ellipse";

  static SVGEllipseElement Create(SVGDocument& document);

  void setCx(Lengthd value);
  void setCy(Lengthd value);
  void setRx(std::optional<Lengthd> value);
  void setRy(std::optional<Lengthd> value);

  Lengthd cx() const;
  Lengthd cy() const;
  std::optional<Lengthd> rx() const;
  std::optional<Lengthd> ry() const;

  Lengthd computedCx() const;
  Lengthd computedCy() const;
  Lengthd computedRx() const;
  Lengthd computedRy() const;

private:
  void invalidate() const;
  void compute() const;
};

}  // namespace donner::svg
