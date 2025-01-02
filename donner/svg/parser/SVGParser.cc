#include "donner/svg/parser/SVGParser.h"

#include <ostream>
#include <sstream>
#include <string_view>
#include <tuple>

#include "donner/base/RcString.h"
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
std::optional<ParseError> ParseNodeContents(SVGParserContext& context, T element,
                                            const XMLNode& node) {
  return std::nullopt;
}

template <>
std::optional<ParseError> ParseNodeContents<SVGStyleElement>(SVGParserContext& context,
                                                             SVGStyleElement element,
                                                             const XMLNode& node) {
  if (element.isCssType()) {
    for (auto child = node.firstChild(); child; child = child->nextSibling()) {
      if (child->type() == XMLNode::Type::Data || child->type() == XMLNode::Type::CData) {
        if (auto value = child->value()) {
          element.setContents(value.value());
        }
      } else {
        ParseError err;
        std::ostringstream ss;
        ss << "Unexpected <style> element contents, expected text or CDATA, "
              "found '"
           << child->type() << "'";

        err.reason = ss.str();
        if (auto sourceOffset = child->sourceStartOffset()) {
          err.location = sourceOffset.value();
        }
        return err;
      }
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
        ParseError err;
        err.reason = "Unexpected namespace '" + value.value() + "'";
        // TODO: Offset for attributes?
        context.addSubparserWarning(std::move(err), ParserOrigin(0));
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
      ParseError err;
      std::ostringstream ss;
      ss << "Ignored attribute '" << attributeName << "' with an unsupported namespace";
      err.reason = ss.str();
      if (auto sourceOffset = node.sourceStartOffset()) {
        err.location = sourceOffset.value();
      }

      // TODO: Offset for attributes?
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

  std::optional<ParseError> walkChildren(std::optional<SVGElement> element,
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
          ParseError err;
          std::ostringstream ss;
          ss << "Ignored element <" << name << "> with an unsupported namespace. "
             << "Expected '" << context_.namespacePrefix() << "', found '" << name.namespacePrefix
             << "'";
          err.reason = ss.str();
          if (auto sourceOffset = child->sourceStartOffset()) {
            err.location = sourceOffset.value();
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
              ParseError err;
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
                err.location = sourceOffset.value();
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
          ParseError err;
          std::ostringstream ss;
          ss << "Unexpected element <" << name << "> at root, first element must be <svg>";
          err.reason = ss.str();
          if (auto sourceOffset = child->sourceStartOffset()) {
            err.location = sourceOffset.value();
          }
          return err;
        }
      }

      child = child->nextSibling();
    }

    return std::nullopt;
  }
};

ParseResult<SVGDocument> SVGParser::ParseSVG(
    std::string_view source, std::vector<ParseError>* outWarnings, SVGParser::Options options,
    std::unique_ptr<ResourceLoaderInterface> resourceLoader) noexcept {
  auto maybeDocument = xml::XMLParser::Parse(source);
  if (maybeDocument.hasError()) {
    return std::move(maybeDocument.error());
  }

  SVGDocument::Settings settings;
  settings.resourceLoader = std::move(resourceLoader);

  xml::XMLDocument xmlDocument(maybeDocument.result());

  SVGParserContext context(source, outWarnings, options);
  SVGParserImpl parser(context, xmlDocument.sharedRegistry(), std::move(settings));
  if (auto error = parser.walkChildren(std::nullopt, xmlDocument.root())) {
    return std::move(error.value());
  }

  if (auto maybeDocument = parser.document()) {
    return std::move(maybeDocument.value());
  } else {
    ParseError err;
    err.reason = "No SVG element found in document";
    err.location = FileOffset::Offset(0);
    return err;
  }
}

ParseResult<SVGDocument> SVGParser::ParseXMLDocument(
    xml::XMLDocument&& xmlDocument, std::vector<ParseError>* outWarnings,
    SVGParser::Options options, std::unique_ptr<ResourceLoaderInterface> resourceLoader) noexcept {
  SVGDocument::Settings settings;
  settings.resourceLoader = std::move(resourceLoader);

  SVGParserContext context(std::string_view(), outWarnings, options);
  SVGParserImpl parser(context, xmlDocument.sharedRegistry(), std::move(settings));
  if (auto error = parser.walkChildren(std::nullopt, xmlDocument.root())) {
    return std::move(error.value());
  }

  if (auto maybeDocument = parser.document()) {
    return std::move(maybeDocument.value());
  } else {
    ParseError err;
    err.reason = "No SVG element found in document";
    err.location = FileOffset::Offset(0);
    return err;
  }
}

}  // namespace donner::svg::parser
