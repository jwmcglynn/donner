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
ParseResult<bool> ParsePresentationAttribute<ElementType::FeBlend>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::FeComponentTransfer>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::FeFuncA>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::FeFuncB>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::FeFuncG>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::FeFuncR>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::FeColorMatrix>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::FeComposite>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

// ParsePresentationAttribute<FeFlood> is defined in FilterPrimitiveComponent.cc.
// ParsePresentationAttribute<FeDropShadow> is defined in FilterPrimitiveComponent.cc.

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::FeGaussianBlur>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::FeMerge>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::FeMergeNode>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::FeMorphology>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::FeDisplacementMap>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::FeConvolveMatrix>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::FeTile>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::FeTurbulence>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::FeOffset>(
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
