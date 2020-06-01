#include "src/svg/xml/xml_parser.h"

#include <iostream>
#include <rapidxml_ns/rapidxml_ns.hpp>
#include <string_view>

#include "src/svg/svg_element.h"

namespace donner {
namespace {
std::string_view TypeToString(rapidxml_ns::node_type type) {
  switch (type) {
    case rapidxml_ns::node_document: return "node_document";
    case rapidxml_ns::node_element: return "node_element";
    case rapidxml_ns::node_data: return "node_data";
    case rapidxml_ns::node_cdata: return "node_cdata";
    case rapidxml_ns::node_comment: return "node_comment";
    case rapidxml_ns::node_declaration: return "node_declaration";
    case rapidxml_ns::node_doctype: return "node_doctype";
    case rapidxml_ns::node_pi: return "node_pi";
  }
}

std::optional<ParseError> ParseOptions(SVGElement element, rapidxml_ns::xml_node<>* node) {
  for (rapidxml_ns::xml_attribute<>* i = node->first_attribute(); i; i = i->next_attribute()) {
    const std::string_view name = std::string_view(i->name(), i->name_size());
    const std::string_view value = std::string_view(i->value(), i->value_size());

    if (name == "id") {
      element.setId(value);
    }
  }

  return std::nullopt;
}

std::optional<ParseError> WalkChildren(SVGDocument& svgDocument, std::optional<SVGElement> element,
                                       rapidxml_ns::xml_node<>* rootNode) {
  bool foundRootSvg = false;

  for (rapidxml_ns::xml_node<>* i = rootNode->first_node(); i; i = i->next_sibling()) {
    const std::string_view name = std::string_view(i->name(), i->name_size());
    std::cout << TypeToString(i->type()) << ": " << std::string_view(i->prefix(), i->prefix_size())
              << name << std::endl;

    if (i->type() == rapidxml_ns::node_element) {
      if (element) {
        auto unknownElement = SVGUnknownElement::Create(svgDocument);
        if (auto error = ParseOptions(unknownElement, i)) {
          return error;
        }
        element->appendChild(unknownElement);

        WalkChildren(svgDocument, unknownElement, i);
      } else {
        // First node must be SVG.
        if (name == "svg" && !foundRootSvg) {
          auto svgElement = svgDocument.svgElement();
          ParseOptions(svgElement, i);
          if (auto error = ParseOptions(svgElement, i)) {
            return error;
          }
          foundRootSvg = true;

          WalkChildren(svgDocument, svgElement, i);
        } else {
          ParseError err;
          err.reason = foundRootSvg ? ("Unexpected element <" + std::string(name) + "> at root")
                                    : "First element must be <svg>";
          return err;
        }
      }
    }
  }

  return std::nullopt;
}
}  // namespace

ParseResult<SVGDocument> XMLParser::parseSVG(std::span<char> str) {
  const int flags = rapidxml_ns::parse_full | rapidxml_ns::parse_trim_whitespace |
                    rapidxml_ns::parse_normalize_whitespace;

  rapidxml_ns::xml_document<> xmlDocument;
  try {
    xmlDocument.parse<flags>(str.data());
  } catch (rapidxml_ns::parse_error& e) {
    ParseError err;
    err.reason = e.what();
    err.offset = e.where<char>() - str.data();
    return err;
  }

  SVGDocument svgDocument;
  if (auto error = WalkChildren(svgDocument, std::nullopt, &xmlDocument)) {
    return std::move(error.value());
  }

  return svgDocument;
}

}  // namespace donner