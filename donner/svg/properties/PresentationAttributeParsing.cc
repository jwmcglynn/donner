#include "donner/svg/properties/PresentationAttributeParsing.h"

namespace donner::svg::parser {

// For elements without components, define the presentation attribute template overload for them
// here.
template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Defs>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::G>(EntityHandle handle,
                                                             std::string_view name,
                                                             const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Unknown>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Filter>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::FeGaussianBlur>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::ClipPath>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // In SVG2, <clipPath> still has normal attributes, not presentation attributes that can be
  // specified in CSS.
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Marker>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // In SVG2, <marker> still has normal attributes, not presentation attributes that can be
  // specified in CSS.
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Mask>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // In SVG2, <mask> still has normal attributes, not presentation attributes that can be
  // specified in CSS.
  return false;
}

}  // namespace donner::svg::parser
