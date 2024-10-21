#pragma once

#include <gmock/gmock.h>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg {

/// The default size of SVG images instantiated by \ref instantiateSubtree, \ref
/// instantiateSubtreeElement, or \ref instantiateSubtreeElementAs.
static constexpr Vector2i kTestSvgDefaultSize = {16, 16};

SVGDocument instantiateSubtree(std::string_view str, const parser::SVGParser::Options& options = {},
                               const Vector2i& size = kTestSvgDefaultSize);

template <typename ElementT = SVGElement>
struct ParsedFragment {
  SVGDocument document;
  ElementT element;

  operator ElementT() { return element; }
  ElementT* const operator->() { return &element; }
};

ParsedFragment<> instantiateSubtreeElement(std::string_view str,
                                           const parser::SVGParser::Options& options = {},
                                           Vector2i size = kTestSvgDefaultSize);

template <typename ElementT>
ParsedFragment<ElementT> instantiateSubtreeElementAs(std::string_view str,
                                                     const parser::SVGParser::Options& options = {},
                                                     Vector2i size = kTestSvgDefaultSize) {
  auto result = instantiateSubtreeElement(str, options, size);

  return ParsedFragment<ElementT>{std::move(result.document), result.element.cast<ElementT>()};
};

}  // namespace donner::svg
