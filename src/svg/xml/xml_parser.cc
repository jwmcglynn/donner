#include "src/svg/xml/xml_parser.h"

#include <iostream>
#include <rapidxml_ns/rapidxml_ns.hpp>
#include <string_view>
#include <tuple>

#include "src/base/parser/length_parser.h"
#include "src/svg/parser/preserve_aspect_ratio_parser.h"
#include "src/svg/parser/transform_parser.h"
#include "src/svg/parser/viewbox_parser.h"
#include "src/svg/svg_element.h"
#include "src/svg/svg_path_element.h"
#include "src/svg/svg_rect_element.h"
#include "src/svg/svg_style_element.h"
#include "src/svg/svg_svg_element.h"
#include "src/svg/svg_unknown_element.h"
#include "src/svg/xml/details/xml_parser_context.h"

namespace donner {

namespace {

using SVGElements = entt::type_list<SVGSVGElement, SVGPathElement, SVGRectElement, SVGStyleElement>;

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

static std::optional<Lengthd> ParseLengthAttribute(XMLParserContext& context,
                                                   std::string_view value) {
  LengthParser::Options options;
  options.unitOptional = true;

  auto maybeLengthResult = LengthParser::Parse(value, options);
  if (maybeLengthResult.hasError()) {
    context.addSubparserWarning(std::move(maybeLengthResult.error()),
                                context.parserOriginFrom(value));
    return std::nullopt;
  }

  if (maybeLengthResult.result().consumed_chars != value.size()) {
    ParseError err;
    err.reason = "Unexpected data at end of attribute";
    err.offset = maybeLengthResult.result().consumed_chars;
    context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    return std::nullopt;
  }

  return maybeLengthResult.result().length;
}

static std::optional<ParseError> ParseCommonAttribute(XMLParserContext& context, SVGElement element,
                                                      std::string_view namespacePrefix,
                                                      std::string_view name,
                                                      std::string_view value) {
  if (name == "id") {
    element.setId(value);
  } else if (name == "class") {
    element.setClassName(value);
  } else if (name == "transform") {
    auto maybeTransform = TransformParser::Parse(value);
    if (maybeTransform.hasError()) {
      context.addSubparserWarning(std::move(maybeTransform.error()),
                                  context.parserOriginFrom(value));
    } else {
      element.setTransform(maybeTransform.result());
    }
  } else if (name == "style") {
    element.setStyle(value);
  } else {
    // Try to parse as a presentation attribute.
    if (!element.trySetPresentationAttribute(name, value)) {
      ParseError err;
      err.reason = "Unknown attribute '" + std::string(name) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  }
  return std::nullopt;
}

template <typename T>
std::optional<ParseError> ParseNodeContents(XMLParserContext& context, T element,
                                            rapidxml_ns::xml_node<>* node) {
  return std::nullopt;
}

template <typename T>
std::optional<ParseError> ParseAttribute(XMLParserContext& context, T element,
                                         std::string_view namespacePrefix, std::string_view name,
                                         std::string_view value) {
  return ParseCommonAttribute(context, element, namespacePrefix, name, value);
}

template <>
std::optional<ParseError> ParseAttribute<SVGPathElement>(XMLParserContext& context,
                                                         SVGPathElement element,
                                                         std::string_view namespacePrefix,
                                                         std::string_view name,
                                                         std::string_view value) {
  if (name == "d") {
    if (auto warning = element.setD(value)) {
      context.addSubparserWarning(std::move(warning.value()), context.parserOriginFrom(value));
    }
  } else {
    return ParseCommonAttribute(context, element, namespacePrefix, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGSVGElement>(XMLParserContext& context,
                                                        SVGSVGElement element,
                                                        std::string_view namespacePrefix,
                                                        std::string_view name,
                                                        std::string_view value) {
  if (name == "viewBox") {
    auto maybeViewbox = ViewboxParser::Parse(value);
    if (maybeViewbox.hasError()) {
      context.addSubparserWarning(std::move(maybeViewbox.error()), context.parserOriginFrom(value));
    } else {
      element.setViewbox(maybeViewbox.result());
    }
  } else if (name == "preserveAspectRatio") {
    auto maybeAspectRatio = PreserveAspectRatioParser::Parse(value);
    if (maybeAspectRatio.hasError()) {
      context.addSubparserWarning(std::move(maybeAspectRatio.error()),
                                  context.parserOriginFrom(value));
    } else {
      element.setPreserveAspectRatio(maybeAspectRatio.result());
    }
  } else if (name == "x") {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setX(length.value());
    }
  } else if (name == "y") {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setY(length.value());
    }
  } else if (name == "width") {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setWidth(length.value());
    }
  } else if (name == "height") {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setHeight(length.value());
    }
  } else if (namespacePrefix == "xmlns" || name == "xmlns") {
    // This was already parsed by @ref ParseXmlNsAttribute.
  } else {
    return ParseCommonAttribute(context, element, namespacePrefix, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGRectElement>(XMLParserContext& context,
                                                         SVGRectElement element,
                                                         std::string_view namespacePrefix,
                                                         std::string_view name,
                                                         std::string_view value) {
  if (name == "x") {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setX(length.value());
    }
  } else if (name == "y") {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setY(length.value());
    }
  } else if (name == "width") {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setWidth(length.value());
    }
  } else if (name == "height") {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setHeight(length.value());
    }
  } else if (name == "rx") {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setRx(length.value());
    }
  } else if (name == "ry") {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setRy(length.value());
    }
  } else {
    return ParseCommonAttribute(context, element, namespacePrefix, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGStyleElement>(XMLParserContext& context,
                                                          SVGStyleElement element,
                                                          std::string_view namespacePrefix,
                                                          std::string_view name,
                                                          std::string_view value) {
  if (name == "type") {
    if (value.empty() ||
        StringUtils::Equals<StringComparison::IgnoreCase>(value, std::string_view("text/css"))) {
      // The value is valid.
    } else {
      ParseError err;
      err.reason = "Invalid <style> element type '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }

    element.setType(RcString(value));
  } else {
    return ParseCommonAttribute(context, element, namespacePrefix, name, value);
  }

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
      }

      if (namespacePrefix == "xmlns") {
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

    if (!namespacePrefix.empty() && namespacePrefix != "xmlns") {
      ParseError err;
      err.reason = "Ignored attribute '" + std::string(i->name(), i->name_size()) +
                   "' with an unsupported namespace";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(namespacePrefix));
      continue;
    }

    if (auto error = ParseAttribute(context, element, namespacePrefix, name, value)) {
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
                                      std::string_view tagName, rapidxml_ns::xml_node<>* node,
                                      entt::type_list<Types...>) {
  if constexpr (I != sizeof...(Types)) {
    if (tagName == std::tuple_element<I, std::tuple<Types...>>::type::Tag) {
      return ParseAttributes(
          context, std::tuple_element<I, std::tuple<Types...>>::type::Create(svgDocument), node);
    }

    return CreateElement<I + 1>(context, svgDocument, tagName, node, entt::type_list<Types...>());
  } else {
    return ParseAttributes(context, SVGUnknownElement::Create(svgDocument, RcString(tagName)),
                           node);
  }
}

std::optional<ParseError> WalkChildren(XMLParserContext& context, SVGDocument& svgDocument,
                                       std::optional<SVGElement> element,
                                       rapidxml_ns::xml_node<>* rootNode) {
  bool foundRootSvg = false;

  for (rapidxml_ns::xml_node<>* i = rootNode->first_node(); i; i = i->next_sibling()) {
    const std::string_view name = std::string_view(i->local_name(), i->local_name_size());
    const std::string_view namespacePrefix = std::string_view(i->prefix(), i->prefix_size());
    std::cout << TypeToString(i->type()) << ": " << std::string_view(i->name(), i->name_size())
              << std::endl;

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

        auto maybeNewElement = CreateElement(context, svgDocument, name, i, SVGElements());
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
                         "> has a mismatched namespace prefix";
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
                                             std::vector<ParseError>* outWarnings) {
  const int flags = rapidxml_ns::parse_full | rapidxml_ns::parse_trim_whitespace |
                    rapidxml_ns::parse_normalize_whitespace;

  XMLParserContext context(std::string_view(str.data(), str.size()), outWarnings);

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

}  // namespace donner
