#include "donner/base/xml/XMLIncrementalParser.h"

#include <string>

#include "donner/base/FileOffset.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/xml/XMLParser.h"

namespace donner::xml {

ParseResult<XMLDocument> XMLIncrementalParser::ParseAttribute(std::string_view attributeSource) {
  std::string fragment;
  fragment.reserve(attributeSource.size() + 9);
  fragment.append("<node ");
  fragment.append(attributeSource);
  fragment.append("/>");
  return XMLParser::Parse(fragment);
}

ParseResult<XMLDocument> XMLIncrementalParser::ParseOpeningTag(std::string_view openingTagSource) {
  if (openingTagSource.empty() || openingTagSource.back() != '>') {
    return ParseDiagnostic::Error("Opening tag is missing '>'", FileOffset::Offset(0));
  }

  std::string fragment(openingTagSource);
  if (fragment.size() < 2 || fragment[fragment.size() - 2] != '/') {
    fragment.insert(fragment.end() - 1, '/');
  }

  return XMLParser::Parse(fragment);
}

ParseResult<XMLDocument> XMLIncrementalParser::ParsePcdata(std::string_view textSource) {
  std::string fragment;
  fragment.reserve(textSource.size() + 13);
  fragment.append("<node>");
  fragment.append(textSource);
  fragment.append("</node>");
  return XMLParser::Parse(fragment);
}

ParseResult<XMLDocument> XMLIncrementalParser::ParseTextLikeNode(std::string_view nodeSource) {
  return XMLParser::Parse(nodeSource, XMLParser::Options::ParseAll());
}

ParseResult<XMLDocument> XMLIncrementalParser::ParseElement(std::string_view elementSource) {
  return XMLParser::Parse(elementSource);
}

}  // namespace donner::xml
