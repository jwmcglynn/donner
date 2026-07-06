#include "donner/svg/properties/CssStringifier.h"

#include <sstream>
#include <string_view>

#include "donner/base/FormatNumber.h"
#include "donner/base/RcString.h"

namespace donner::svg {

std::string CssSerialize(Display value) {
  // Display's operator<< already emits the CSS keyword (`inline`, `none`, ...).
  std::ostringstream os;
  os << value;
  return os.str();
}

std::string CssSerialize(Visibility value) {
  // Visibility's operator<< already emits the CSS keyword.
  std::ostringstream os;
  os << value;
  return os.str();
}

std::string CssSerialize(double value) {
  return donner::detail::FormatNumberForSVG(value);
}

std::string CssSerialize(const Lengthd& value) {
  const RcString text = value.toRcString();
  return std::string(std::string_view(text));
}

std::string CssSerialize(const css::Color& value) {
  if (value.isCurrentColor()) {
    return "currentColor";
  }
  return value.asRGBA().toHexString();
}

std::string CssSerialize(const PaintServer& value) {
  if (value.is<PaintServer::None>()) {
    return "none";
  }
  if (value.is<PaintServer::ContextFill>()) {
    return "context-fill";
  }
  if (value.is<PaintServer::ContextStroke>()) {
    return "context-stroke";
  }
  if (value.is<PaintServer::Solid>()) {
    return CssSerialize(value.get<PaintServer::Solid>().color);
  }

  const PaintServer::ElementReference& ref = value.get<PaintServer::ElementReference>();
  std::string out = "url(";
  out += std::string_view(ref.reference.href);
  out += ")";
  if (ref.fallback.has_value()) {
    out += " ";
    out += CssSerialize(*ref.fallback);
  }
  return out;
}

}  // namespace donner::svg
