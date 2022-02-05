#pragma once

#include <gmock/gmock.h>

#include "src/svg/svg_document.h"
#include "src/svg/svg_element.h"

namespace donner {

SVGDocument instantiateSubtree(std::string_view str);

template <typename ElementT = SVGElement>
struct ParsedFragment {
  SVGDocument document;
  ElementT element;

  operator ElementT() { return element; }
  ElementT* const operator->() { return &element; }
};

ParsedFragment<> instantiateSubtreeElement(std::string_view str);

template <typename ElementT>
ParsedFragment<ElementT> instantiateSubtreeElementAs(std::string_view str) {
  auto result = instantiateSubtreeElement(str);

  return ParsedFragment<ElementT>{std::move(result.document), result.element.cast<ElementT>()};
};

}  // namespace donner
