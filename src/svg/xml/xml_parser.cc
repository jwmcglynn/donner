#include "src/svg/xml/xml_parser.h"

#include <rapidxml_ns/rapidxml_ns.hpp>
#include <string_view>
#include <tuple>

#include "src/svg/all_svg_elements.h"
#include "src/svg/xml/attribute_parser.h"
#include "src/svg/xml/details/xml_parser_context.h"
#include "src/svg/xml/xml_qualified_name.h"

namespace donner::svg::parser {

namespace {

template <typename T>
concept HasPathLength =
    requires(T element, std::optional<double> value) { element.setPathLength(value); };

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

template <typename T>
std::optional<ParseError> ParseNodeContents(XMLParserContext& context, T element,
                                            rapidxml_ns::xml_node<>* node) {
  return std::nullopt;
}

template <>
std::optional<ParseError> ParseNodeContents<SVGStyleElement>(XMLParserContext& context,
                                                             SVGStyleElement element,
                                                             rapidxml_ns::xml_node<>* node) {
  if (element.isCssType()) {
    for (rapidxml_ns::xml_node<>* i = node->first_node(); i; i = i->next_sibling()) {
      if (i->type() == rapidxml_ns::node_data || i->type() == rapidxml_ns::node_cdata) {
        element.setContents(std::string_view(i->value(), i->value_size()));
      } else {
        ParseError err;
        err.reason =
            std::string("Unexpected <style> element contents, expected text or CDATA, found '") +
            std::string(TypeToString(i->type())) + "'";
        err.offset =
            context.parserOriginFrom(std::string_view(node->name(), node->name_size())).startOffset;
        return err;
      }
    }
  }

  return std::nullopt;
}

void ParseXmlNsAttribute(XMLParserContext& context, rapidxml_ns::xml_node<>* node) {
  for (rapidxml_ns::xml_attribute<>* i = node->first_attribute(); i; i = i->next_attribute()) {
    const std::string_view name = std::string_view(i->local_name(), i->local_name_size());
    const std::string_view namespacePrefix = std::string_view(i->prefix(), i->prefix_size());
    const std::string_view value = std::string_view(i->value(), i->value_size());

    if (name == "xmlns" || namespacePrefix == "xmlns") {
      // We need to handle the namespacePrefix special for handling for xmlns, which may be in the
      // format of `xmlns:namespace`, swapping the name with the namespace.
      if (value != "http://www.w3.org/2000/svg") {
        ParseError err;
        err.reason = "Unexpected namespace '" + std::string(value) + "'";
        context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
      } else if (namespacePrefix == "xmlns") {
        context.setNamespacePrefix(name);
      }

      break;
    }
  }
}

template <typename T>
ParseResult<SVGElement> ParseAttributes(XMLParserContext& context, T element,
                                        rapidxml_ns::xml_node<>* node) {
  for (rapidxml_ns::xml_attribute<>* i = node->first_attribute(); i; i = i->next_attribute()) {
    const std::string_view namespacePrefix = std::string_view(i->prefix(), i->prefix_size());
    const std::string_view name = std::string_view(i->local_name(), i->local_name_size());
    const std::string_view value = std::string_view(i->value(), i->value_size());

    if (!namespacePrefix.empty() && namespacePrefix != "xmlns" && namespacePrefix != "xlink") {
      ParseError err;
      err.reason = "Ignored attribute '" + std::string(i->name(), i->name_size()) +
                   "' with an unsupported namespace";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(namespacePrefix));
      continue;
    }

    if (auto error = AttributeParser::ParseAndSetAttribute(
            context, element, XMLQualifiedNameRef(namespacePrefix, name), value)) {
      return std::move(error.value());
    }
  }

  if (auto error = ParseNodeContents(context, element, node)) {
    return std::move(error.value());
  }

  return std::move(element);
}

template <size_t I = 0, typename... Types>
ParseResult<SVGElement> CreateElement(XMLParserContext& context, SVGDocument& svgDocument,
                                      const svg::XMLQualifiedNameRef& tagName,
                                      rapidxml_ns::xml_node<>* node, entt::type_list<Types...>) {
  if constexpr (I != sizeof...(Types)) {
    if (tagName == std::tuple_element<I, std::tuple<Types...>>::type::Tag) {
      return ParseAttributes(
          context, std::tuple_element<I, std::tuple<Types...>>::type::Create(svgDocument), node);
    }

    return CreateElement<I + 1>(context, svgDocument, tagName, node, entt::type_list<Types...>());
  } else {
    return ParseAttributes(context, SVGUnknownElement::Create(svgDocument, tagName), node);
  }
}

std::optional<ParseError> WalkChildren(XMLParserContext& context, SVGDocument& svgDocument,
                                       std::optional<SVGElement> element,
                                       rapidxml_ns::xml_node<>* rootNode) {
  bool foundRootSvg = false;

  for (rapidxml_ns::xml_node<>* i = rootNode->first_node(); i; i = i->next_sibling()) {
    const std::string_view name = std::string_view(i->local_name(), i->local_name_size());
    const std::string_view namespacePrefix = std::string_view(i->prefix(), i->prefix_size());

    if (i->type() == rapidxml_ns::node_element) {
      if (element) {
        // TODO: Create an SVGUnknownElement if the namespace doesn't match?
        if (namespacePrefix != context.namespacePrefix()) {
          ParseError err;
          err.reason = "Ignored element <" + std::string(i->name(), i->name_size()) +
                       "> with an unsupported namespace";
          context.addSubparserWarning(std::move(err), context.parserOriginFrom(namespacePrefix));
          continue;
        }

        auto maybeNewElement = CreateElement(context, svgDocument, name, i, AllSVGElements());
        if (maybeNewElement.hasError()) {
          return std::move(maybeNewElement.error());
        }

        element->appendChild(maybeNewElement.result());
        if (auto error = WalkChildren(context, svgDocument, maybeNewElement.result(), i)) {
          return error;
        }
      } else {
        // First node must be SVG.
        if (name == "svg" && !foundRootSvg) {
          ParseXmlNsAttribute(context, i);

          auto maybeSvgElement = ParseAttributes(context, svgDocument.svgElement(), i);
          if (maybeSvgElement.hasError()) {
            return std::move(maybeSvgElement.error());
          }

          if (namespacePrefix != context.namespacePrefix()) {
            ParseError err;
            err.reason = "<" + std::string(i->name(), i->name_size()) +
                         "> has a mismatched namespace prefix. Expected '" +
                         std::string(context.namespacePrefix()) + "', found '" +
                         std::string(namespacePrefix) + "'";
            return context.fromSubparser(std::move(err), context.parserOriginFrom(namespacePrefix));
          }

          foundRootSvg = true;
          if (auto error = WalkChildren(context, svgDocument, maybeSvgElement.result(), i)) {
            return error;
          }
        } else {
          ParseError err;
          err.reason =
              "Unexpected element <" + std::string(name) + "> at root, first element must be <svg>";
          return err;
        }
      }
    }
  }

  return std::nullopt;
}
}  // namespace

ParseResult<SVGDocument> XMLParser::ParseSVG(std::span<char> str,
                                             std::vector<ParseError>* outWarnings,
                                             XMLParser::Options options) noexcept {
  const int flags = rapidxml_ns::parse_full | rapidxml_ns::parse_trim_whitespace |
                    rapidxml_ns::parse_normalize_whitespace;

  XMLParserContext context(std::string_view(str.data(), str.size()), outWarnings, options);

  rapidxml_ns::xml_document<> xmlDocument;
  try {
    xmlDocument.parse<flags>(str.data());
  } catch (rapidxml_ns::parse_error& e) {
    const size_t offset = e.where<char>() - str.data();

    ParseError err;
    err.reason = e.what();
    err.line = context.offsetToLine(offset);
    err.offset = offset - context.lineOffset(err.line);
    return err;
  }

  SVGDocument svgDocument;
  if (auto error = WalkChildren(context, svgDocument, std::nullopt, &xmlDocument)) {
    return std::move(error.value());
  }

  return svgDocument;
}

}  // namespace donner::svg::parser
