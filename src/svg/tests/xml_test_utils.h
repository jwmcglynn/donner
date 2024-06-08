#pragma once

#include <gmock/gmock.h>

#include "src/svg/svg_document.h"
#include "src/svg/svg_element.h"
#include "src/svg/xml/xml_parser.h"

namespace donner::svg {

SVGDocument instantiateSubtree(std::string_view str, const XMLParser::Options& options = {});

template <typename ElementT = SVGElement>
struct ParsedFragment {
  SVGDocument document;
  ElementT element;

  operator ElementT() { return element; }
  ElementT* const operator->() { return &element; }
};

ParsedFragment<> instantiateSubtreeElement(std::string_view str,
                                           const XMLParser::Options& options = {});

template <typename ElementT>
ParsedFragment<ElementT> instantiateSubtreeElementAs(std::string_view str,
                                                     const XMLParser::Options& options = {}) {
  auto result = instantiateSubtreeElement(str, options);

  return ParsedFragment<ElementT>{std::move(result.document), result.element.cast<ElementT>()};
};

}  // namespace donner::svg
