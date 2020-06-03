#include "src/svg/xml/xml_parser.h"

#include <iostream>
#include <rapidxml_ns/rapidxml_ns.hpp>
#include <string_view>
#include <tuple>

#include "src/svg/svg_element.h"

namespace donner {

namespace {

using SVGElements = entt::type_list<SVGSVGElement, SVGPathElement>;

std::optional<ParseError> ParseCommonAttribute(SVGElement element, std::string_view name,
                                               std::string_view value,
                                               std::vector<ParseError>* out_warnings) {
  if (name == "id") {
    element.setId(value);
  }
  return std::nullopt;
}

template <typename T>
std::optional<ParseError> ParseAttribute(T element, std::string_view name, std::string_view value,
                                         std::vector<ParseError>* out_warnings) {
  return ParseCommonAttribute(element, name, value, out_warnings);
}

template <>
std::optional<ParseError> ParseAttribute<SVGPathElement>(SVGPathElement element,
                                                         std::string_view name,
                                                         std::string_view value,
                                                         std::vector<ParseError>* out_warnings) {
  if (name == "d") {
    if (auto warning = element.setD(value)) {
      if (out_warnings) {
        out_warnings->emplace_back(std::move(warning.value()));
      }
    }

    std::cout << "d = " << element.d() << std::endl;
  }

  return ParseCommonAttribute(element, name, value, out_warnings);
}

template <typename T>
ParseResult<SVGElement> ParseAttributes(T element, rapidxml_ns::xml_node<>* node,
                                        std::vector<ParseError>* out_warnings) {
  for (rapidxml_ns::xml_attribute<>* i = node->first_attribute(); i; i = i->next_attribute()) {
    const std::string_view name = std::string_view(i->name(), i->name_size());
    const std::string_view value = std::string_view(i->value(), i->value_size());
    if (auto error = ParseAttribute(element, name, value, out_warnings)) {
      return std::move(error.value());
    }
  }

  return std::move(element);
}

template <size_t I = 0, typename... Types>
ParseResult<SVGElement> CreateElement(SVGDocument& svgDocument, std::string_view tagName,
                                      rapidxml_ns::xml_node<>* node, entt::type_list<Types...>,
                                      std::vector<ParseError>* out_warnings) {
  if constexpr (I != sizeof...(Types)) {
    if (tagName == std::tuple_element<I, std::tuple<Types...>>::type::Tag) {
      return ParseAttributes(std::tuple_element<I, std::tuple<Types...>>::type::Create(svgDocument),
                             node, out_warnings);
    }

    return CreateElement<I + 1>(svgDocument, tagName, node, entt::type_list<Types...>(),
                                out_warnings);
  } else {
    return ParseAttributes(SVGUnknownElement::Create(svgDocument), node, out_warnings);
  }
}

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

std::optional<ParseError> WalkChildren(SVGDocument& svgDocument, std::optional<SVGElement> element,
                                       rapidxml_ns::xml_node<>* rootNode,
                                       std::vector<ParseError>* out_warnings) {
  bool foundRootSvg = false;

  for (rapidxml_ns::xml_node<>* i = rootNode->first_node(); i; i = i->next_sibling()) {
    const std::string_view name = std::string_view(i->name(), i->name_size());
    std::cout << TypeToString(i->type()) << ": " << std::string_view(i->prefix(), i->prefix_size())
              << name << std::endl;

    if (i->type() == rapidxml_ns::node_element) {
      if (element) {
        auto maybeNewElement = CreateElement(svgDocument, name, i, SVGElements(), out_warnings);
        if (maybeNewElement.hasError()) {
          return std::move(maybeNewElement.error());
        }

        element->appendChild(maybeNewElement.result());
        if (auto error = WalkChildren(svgDocument, maybeNewElement.result(), i, out_warnings)) {
          return error;
        }
      } else {
        // First node must be SVG.
        if (name == "svg" && !foundRootSvg) {
          auto maybeSvgElement = ParseAttributes(svgDocument.svgElement(), i, out_warnings);
          if (maybeSvgElement.hasError()) {
            return std::move(maybeSvgElement.error());
          }
          foundRootSvg = true;

          if (auto error = WalkChildren(svgDocument, maybeSvgElement.result(), i, out_warnings)) {
            return error;
          }
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

ParseResult<SVGDocument> XMLParser::parseSVG(std::span<char> str,
                                             std::vector<ParseError>* out_warnings) {
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
  if (auto error = WalkChildren(svgDocument, std::nullopt, &xmlDocument, out_warnings)) {
    return std::move(error.value());
  }

  return svgDocument;
}

}  // namespace donner