#pragma once
/// @file

#include <string_view>

#include "donner/base/ParseResult.h"
#include "donner/base/xml/XMLDocument.h"

namespace donner::xml {

/**
 * Parser entry points for source-edit fragments.
 *
 * These helpers parse the bounded XML scopes used by \ref XMLDocument::applySourceEdit. Returned
 * documents own a temporary source store whose source locations are relative to the parsed
 * fragment; callers that install the parsed metadata into a live document must offset those ranges
 * by the fragment's source offset.
 */
class XMLIncrementalParser {
public:
  /**
   * Parse one attribute source fragment by wrapping it in a synthetic element.
   *
   * @param attributeSource Source bytes such as `fill="red"`.
   * @return Parsed temporary XML document, or a parse diagnostic.
   */
  static ParseResult<XMLDocument> ParseAttribute(std::string_view attributeSource);

  /**
   * Parse one opening tag source fragment.
   *
   * The opening tag is converted to a self-closing tag when needed so attributes can be parsed
   * without reparsing the element body.
   *
   * @param openingTagSource Source bytes such as `<rect fill="red">`.
   * @return Parsed temporary XML document, or a parse diagnostic.
   */
  static ParseResult<XMLDocument> ParseOpeningTag(std::string_view openingTagSource);

  /**
   * Parse parsed-character-data source by wrapping it in a synthetic element.
   *
   * @param textSource Source bytes from an element text span.
   * @return Parsed temporary XML document, or a parse diagnostic.
   */
  static ParseResult<XMLDocument> ParsePcdata(std::string_view textSource);

  /**
   * Parse one raw text-like node source fragment.
   *
   * This is used for CDATA sections, XML comments, and processing instructions.
   *
   * @param nodeSource Source bytes for the complete text-like node.
   * @return Parsed temporary XML document, or a parse diagnostic.
   */
  static ParseResult<XMLDocument> ParseTextLikeNode(std::string_view nodeSource);

  /**
   * Parse one element subtree source fragment.
   *
   * @param elementSource Source bytes for the complete element subtree.
   * @return Parsed temporary XML document, or a parse diagnostic.
   */
  static ParseResult<XMLDocument> ParseElement(std::string_view elementSource);
};

}  // namespace donner::xml
