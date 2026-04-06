#include "donner/svg/parser/SVGParser.h"

#include <ostream>
#include <sstream>
#include <string_view>
#include <tuple>

#include "donner/base/RcString.h"
#include "donner/base/encoding/Decompress.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/svg/AllSVGElements.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/parser/AttributeParser.h"
#include "donner/svg/parser/details/SVGParserContext.h"

namespace donner::svg::parser {

using xml::XMLNode;
using xml::XMLParser;
using xml::XMLQualifiedNameRef;

namespace {

template <typename T>
concept HasPathLength =
    requires(T element, std::optional<double> value) { element.setPathLength(value); };

template <typename T>
std::optional<ParseDiagnostic> ParseNodeContents(SVGParserContext& context, T element,
                                            const XMLNode& node) {
  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseNodeContents<SVGStyleElement>(SVGParserContext& context,
                                                             SVGStyleElement element,
                                                             const XMLNode& node) {
  if (element.isCssType()) {
    // Concatenate all text/CDATA children into a single string before parsing.
    // Multiple Data/CData nodes can occur when whitespace text nodes are preserved
    // between or around CDATA sections.
    std::string combined;
    for (auto child = node.firstChild(); child; child = child->nextSibling()) {
      if (child->type() == XMLNode::Type::Data || child->type() == XMLNode::Type::CData) {
        if (auto value = child->value()) {
          combined += value.value();
        }
      } else {
        ParseDiagnostic err;
        std::ostringstream ss;
        ss << "Unexpected <style> element contents, expected text or CDATA, "
              "found '"
           << child->type() << "'";

        err.reason = ss.str();
        if (auto sourceOffset = child->sourceStartOffset()) {
          err.range.start = sourceOffset.value();
        }
        return err;
      }
    }
    if (!combined.empty()) {
      element.setContents(combined);
    }
  }

  return std::nullopt;
}

/**
 * Parse text content for \ref xml_text elements.
 *
 * @param context The parser context.
 * @param element The text element to parse contents for.
 * @param node The XML node containing the text content.
 * @return std::nullopt if successful, otherwise a ParseDiagnostic describing the failure.
 */
template <>
std::optional<ParseDiagnostic> ParseNodeContents<SVGTextElement>(SVGParserContext& context,
                                                            SVGTextElement element,
                                                            const XMLNode& node) {
  for (auto child = node.firstChild(); child; child = child->nextSibling()) {
    if (child->type() == XMLNode::Type::Data || child->type() == XMLNode::Type::CData) {
      if (auto maybeValue = child->value()) {
        element.appendText(maybeValue.value());
      }
    } else if (child->type() == XMLNode::Type::Element) {
      element.advanceTextChunk();
    }
  }
  return std::nullopt;
}

/**
 * Parse text content for \ref xml_tspan elements.
 *
 * @param context The parser context.
 * @param element The tspan element to parse contents for.
 * @param node The XML node containing the text content.
 * @return std::nullopt if successful, otherwise a ParseDiagnostic describing the failure.
 */
template <>
std::optional<ParseDiagnostic> ParseNodeContents<SVGTSpanElement>(SVGParserContext& context,
                                                             SVGTSpanElement element,
                                                             const XMLNode& node) {
  for (auto child = node.firstChild(); child; child = child->nextSibling()) {
    if (child->type() == XMLNode::Type::Data || child->type() == XMLNode::Type::CData) {
      if (auto maybeValue = child->value()) {
        element.appendText(maybeValue.value());
      }
    } else if (child->type() == XMLNode::Type::Element) {
      element.advanceTextChunk();
    }
  }
  return std::nullopt;
}

/**
 * Parse text content for \ref xml_textPath elements.
 *
 * @param context The parser context.
 * @param element The textPath element to parse contents for.
 * @param node The XML node containing the text content.
 * @return std::nullopt if successful, otherwise a ParseError describing the failure.
 */
template <>
std::optional<ParseError> ParseNodeContents<SVGTextPathElement>(SVGParserContext& context,
                                                                SVGTextPathElement element,
                                                                const XMLNode& node) {
  for (auto child = node.firstChild(); child; child = child->nextSibling()) {
    if (child->type() == XMLNode::Type::Data || child->type() == XMLNode::Type::CData) {
      if (auto maybeValue = child->value()) {
        element.appendText(maybeValue.value());
      }
    } else if (child->type() == XMLNode::Type::Element) {
      element.advanceTextChunk();
    }
  }
  return std::nullopt;
}

void ParseXmlNsAttribute(SVGParserContext& context, const XMLNode& node) {
  bool hasEmptyNamespacePrefix = false;

  for (const auto& attributeName : node.attributes()) {
    if (attributeName == "xmlns" || attributeName.namespacePrefix == "xmlns") {
      // We need to handle the namespacePrefix special for handling for xmlns,
      // which may be in the format of `xmlns:namespace`, swapping the name with
      // the namespace.
      std::optional<RcString> value = node.getAttribute(attributeName);
      assert(value.has_value());

      if (value == "http://www.w3.org/2000/svg") {
        if (!hasEmptyNamespacePrefix && attributeName.namespacePrefix == "xmlns") {
          context.setNamespacePrefix(attributeName.name);
        } else if (attributeName == "xmlns") {
          hasEmptyNamespacePrefix = true;
          context.setNamespacePrefix("");
        }
      } else if (value == "http://www.w3.org/1999/xlink") {
        // Allow xlink.
      } else {
        ParseDiagnostic err;
        err.reason = "Unexpected namespace '" + value.value() + "'";
        if (auto maybeRange = context.getAttributeLocation(node, attributeName)) {
          err.range.start = maybeRange->start;
        }

        context.addWarning(std::move(err));
      }
    }
  }
}

template <typename T>
ParseResult<SVGElement> ParseAttributes(SVGParserContext& context, T element, const XMLNode& node) {
  for (const XMLQualifiedNameRef& attributeName : node.attributes()) {
    const RcString value = node.getAttribute(attributeName).value();

    if (!attributeName.namespacePrefix.empty() && attributeName.namespacePrefix != "xmlns" &&
        attributeName.namespacePrefix != "xlink" &&
        node.getNamespaceUri(attributeName.namespacePrefix) != "http://www.w3.org/2000/svg") {
      ParseDiagnostic err;
      std::ostringstream ss;
      ss << "Ignored attribute '" << attributeName << "' with an unsupported namespace";
      err.reason = ss.str();
      if (auto maybeRange = context.getAttributeLocation(element, attributeName)) {
        err.range.start = maybeRange->start;
      }

      context.addWarning(std::move(err));
      continue;
    }

    if (auto error =
            AttributeParser::ParseAndSetAttribute(context, element, attributeName, value)) {
      return std::move(error.value());
    }
  }

  if (auto error = ParseNodeContents(context, element, node)) {
    return std::move(error.value());
  }

  return std::move(element);
}

/// Returns true if an element type is experimental. Element types opt in by declaring
/// `static constexpr bool IsExperimental = true;`. When a feature ships, remove the
/// `IsExperimental` declaration entirely rather than setting it to false — the absence of the
/// member is the default (non-experimental) state.
template <typename T>
constexpr bool IsExperimental() {
  if constexpr (requires { T::IsExperimental; }) {
    return T::IsExperimental;
  } else {
    return false;
  }
}

}  // namespace

class SVGParserImpl {
private:
  SVGParserContext& context_;
  std::optional<SVGDocument> document_;
  std::shared_ptr<Registry> registry_;
  SVGDocument::Settings settings_;

public:
  explicit SVGParserImpl(SVGParserContext& context, std::shared_ptr<Registry> registry,
                         SVGDocument::Settings settings)
      : context_(context), registry_(std::move(registry)), settings_(std::move(settings)) {}

  std::optional<SVGDocument> document() const { return document_; }

  template <size_t I = 0, typename... Types>
  ParseResult<SVGElement> createElement(const XMLQualifiedNameRef& tagName, const XMLNode& node,
                                        entt::type_list<Types...>) {
    if constexpr (I != sizeof...(Types)) {
      using ElementT = std::tuple_element<I, std::tuple<Types...>>::type;

      // TODO: A faster way to lookup Uris
      if (node.getNamespaceUri(tagName.namespacePrefix) == "http://www.w3.org/2000/svg" &&
          tagName.name == ElementT::Tag) {
        if constexpr (IsExperimental<ElementT>()) {
          if (context_.options().enableExperimental) {
            auto element = ElementT::CreateOn(node.entityHandle());
            return ParseAttributes(context_, element, node);
          } else {
            auto element = SVGUnknownElement::CreateOn(node.entityHandle(), tagName);
            return ParseAttributes(context_, element, node);
          }
        } else {
          auto element = ElementT::CreateOn(node.entityHandle());
          return ParseAttributes(context_, element, node);
        }
      }

      return createElement<I + 1>(tagName, node, entt::type_list<Types...>());
    } else {
      auto element = SVGUnknownElement::CreateOn(node.entityHandle(), tagName);
      return ParseAttributes(context_, element, node);
    }
  }

  std::optional<ParseDiagnostic> walkChildren(std::optional<SVGElement> element,
                                         const XMLNode& rootNode) {
    bool foundRootSvg = false;

    for (auto child = rootNode.firstChild(); child;) {
      const XMLQualifiedNameRef name = child->tagName();

      const XMLNode::Type type = child->type();
      if (type != XMLNode::Type::Element) {
        // Remove the unknown element from the tree.
        XMLNode nodeToRemove = child.value();
        child = child->nextSibling();
        nodeToRemove.remove();
        continue;
      }

      if (element) {
        assert(document_.has_value());

        // TODO: Create an SVGUnknownElement if the namespace doesn't match?
        std::optional<RcString> maybeUri = child->getNamespaceUri(name.namespacePrefix);
        if (maybeUri != "http://www.w3.org/2000/svg") {
          ParseDiagnostic err;
          std::ostringstream ss;
          ss << "Ignored element <" << name << "> with an unsupported namespace. "
             << "Expected '" << context_.namespacePrefix() << "', found '" << name.namespacePrefix
             << "'";
          err.reason = ss.str();
          if (auto sourceOffset = child->sourceStartOffset()) {
            err.range.start = sourceOffset.value();
          }
          context_.addWarning(std::move(err));

          // Remove the unknown element from the tree.
          XMLNode nodeToRemove = child.value();
          child = child->nextSibling();
          nodeToRemove.remove();
          continue;
        }

        auto maybeNewElement = createElement(child->tagName(), child.value(), AllSVGElements());
        if (maybeNewElement.hasError()) {
          return std::move(maybeNewElement.error());
        }

        if (auto error = walkChildren(maybeNewElement.result(), child.value())) {
          return error;
        }
      } else {
        // First node must be SVG.
        if (name.name == "svg" && !foundRootSvg) {
          ParseXmlNsAttribute(context_, *child);

          // Check if this is in the right namespace.
          std::optional<RcString> maybeUri = child->getNamespaceUri(name.namespacePrefix);
          if (maybeUri != "http://www.w3.org/2000/svg") {
            if (context_.options().parseAsInlineSVG && !maybeUri.has_value()) {
              // Inline SVGs don't require the namespace to be set, default to SVG.
              child->setAttribute("xmlns", "http://www.w3.org/2000/svg");
            } else {
              ParseDiagnostic err;
              std::ostringstream ss;
              ss << "<" << name << "> has an ";
              if (maybeUri) {
                ss << "unexpected namespace URI '" << maybeUri.value() << "'. ";
              } else {
                ss << "empty namespace URI. ";
              }
              ss << "Expected 'http://www.w3.org/2000/svg'";
              err.reason = ss.str();
              if (auto sourceOffset = child->sourceStartOffset()) {
                err.range.start = sourceOffset.value();
              }
              return err;
            }
          }

          document_ = SVGDocument(registry_, std::move(settings_), child->entityHandle());

          auto maybeSvgElement = ParseAttributes(context_, document_->svgElement(), child.value());
          if (maybeSvgElement.hasError()) {
            return std::move(maybeSvgElement.error());
          }

          foundRootSvg = true;
          if (auto error = walkChildren(maybeSvgElement.result(), child.value())) {
            return error;
          }
        } else {
          ParseDiagnostic err;
          std::ostringstream ss;
          ss << "Unexpected element <" << name << "> at root, first element must be <svg>";
          err.reason = ss.str();
          if (auto sourceOffset = child->sourceStartOffset()) {
            err.range.start = sourceOffset.value();
          }
          return err;
        }
      }

      child = child->nextSibling();
    }

    return std::nullopt;
  }
};

ParseResult<SVGDocument> SVGParser::ParseSVG(std::string_view source,
                                             std::vector<ParseDiagnostic>* outWarnings,
                                             SVGParser::Options options,
                                             SVGDocument::Settings settings) noexcept {
  // Inject the SVG parse callback for sub-document loading, unless we're already in secure mode
  // (sub-documents cannot load their own sub-documents).
  if (!settings.svgParseCallback && settings.processingMode == ProcessingMode::DynamicInteractive) {
    settings.svgParseCallback =
        [](const std::vector<uint8_t>& svgContent,
           std::vector<ParseDiagnostic>* warnings) -> std::optional<SVGDocumentHandle> {
      SVGDocument::Settings subSettings;
      subSettings.processingMode = ProcessingMode::SecureStatic;
      // No resource loader — secure mode sub-documents cannot load external resources.

      const std::string_view subSource(reinterpret_cast<const char*>(svgContent.data()),
                                       svgContent.size());
      auto result = SVGParser::ParseSVG(subSource, warnings, Options(), std::move(subSettings));
      if (result.hasError()) {
        if (warnings) {
          warnings->emplace_back(result.error());
        }
        return std::nullopt;
      }
      return result.result().handle();
    };
  }

  xml::XMLParser::Options xmlOptions;
  xmlOptions.parseCustomEntities = true;

  std::vector<uint8_t> decompressedData;
  if (source.size() >= 2 && static_cast<unsigned char>(source[0]) == 0x1F &&
      static_cast<unsigned char>(source[1]) == 0x8B) {
    auto maybeDecompressedData = Decompress::Gzip(source);
    if (maybeDecompressedData.hasError()) {
      return std::move(maybeDecompressedData.error());
    }

    decompressedData = std::move(maybeDecompressedData.result());
    source =
        std::string_view(reinterpret_cast<char*>(decompressedData.data()), decompressedData.size());
  }

  auto maybeDocument = xml::XMLParser::Parse(source, xmlOptions);
  if (maybeDocument.hasError()) {
    return std::move(maybeDocument.error());
  }

  xml::XMLDocument xmlDocument(maybeDocument.result());

  SVGParserContext context(source, outWarnings, options);
  SVGParserImpl parser(context, xmlDocument.sharedRegistry(), std::move(settings));
  if (auto error = parser.walkChildren(std::nullopt, xmlDocument.root())) {
    return std::move(error.value());
  }

  if (auto maybeDocument = parser.document()) {
    return std::move(maybeDocument.value());
  } else {
    ParseDiagnostic err;
    err.reason = "No SVG element found in document";
    err.range.start = FileOffset::Offset(0);
    return err;
  }
}

ParseResult<SVGDocument> SVGParser::ParseXMLDocument(xml::XMLDocument&& xmlDocument,
                                                     std::vector<ParseDiagnostic>* outWarnings,
                                                     SVGParser::Options options,
                                                     SVGDocument::Settings settings) noexcept {
  SVGParserContext context(std::string_view(), outWarnings, options);
  SVGParserImpl parser(context, xmlDocument.sharedRegistry(), std::move(settings));
  if (auto error = parser.walkChildren(std::nullopt, xmlDocument.root())) {
    return std::move(error.value());
  }

  if (auto maybeDocument = parser.document()) {
    return std::move(maybeDocument.value());
  } else {
    ParseDiagnostic err;
    err.reason = "No SVG element found in document";
    err.range.start = FileOffset::Offset(0);
    return err;
  }
}

}  // namespace donner::svg::parser
