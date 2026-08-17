#include "donner/svg/parser/SVGParser.h"

#include <array>
#include <ostream>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>

#include "donner/base/CompileTimeMap.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/RcString.h"
#include "donner/base/encoding/Decompress.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/svg/AllSVGElements.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/components/DescriptiveTextComponent.h"
#include "donner/svg/components/StylesheetComponent.h"
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
    components::StylesheetSourceMap sourceMap;
    for (auto child = node.firstChild(); child; child = child->nextSibling()) {
      if (child->type() == XMLNode::Type::Data || child->type() == XMLNode::Type::CData) {
        if (auto value = child->value()) {
          const std::size_t cssStartOffset = combined.size();
          combined += value.value();
          const std::size_t cssEndOffset = combined.size();

          std::optional<SourceRange> childValueLocation = child->getValueLocation();
          if (!childValueLocation.has_value() && child->type() == XMLNode::Type::Data) {
            childValueLocation = child->getNodeLocation();
          }

          if (childValueLocation.has_value() && childValueLocation->start.offset.has_value() &&
              childValueLocation->end.offset.has_value() &&
              *childValueLocation->end.offset >= *childValueLocation->start.offset &&
              *childValueLocation->end.offset - *childValueLocation->start.offset ==
                  value->size()) {
            sourceMap.addSegment(cssStartOffset, cssEndOffset, childValueLocation->start);
          }
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
      auto& stylesheetComponent =
          element.entityHandle().get_or_emplace<components::StylesheetComponent>();
      stylesheetComponent.parseStylesheet(std::string_view(combined), std::move(sourceMap));
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
 * Parse text content for \ref xml_a elements.
 *
 * `<a>` is a transparent text-content group when nested in text, so its direct text children must
 * be captured into the text layout (with chunk boundaries around nested elements) exactly like
 * \ref xml_tspan. Outside of text the captured chunks are inert - the text layout only descends
 * from a text root - so the same handling is safe for the general-container case.
 *
 * @param context The parser context.
 * @param element The `<a>` element to parse contents for.
 * @param node The XML node containing the text content.
 * @return std::nullopt if successful, otherwise a ParseDiagnostic describing the failure.
 */
template <>
std::optional<ParseDiagnostic> ParseNodeContents<SVGAElement>(SVGParserContext& context,
                                                              SVGAElement element,
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
 * @return std::nullopt if successful, otherwise a ParseDiagnostic describing the failure.
 */
template <>
std::optional<ParseDiagnostic> ParseNodeContents<SVGTextPathElement>(SVGParserContext& context,
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

/**
 * Concatenate the direct text/CDATA children of a descriptive element (\ref xml_title, \ref
 * xml_desc, \ref xml_metadata) and store them on a \ref components::DescriptiveTextComponent so the
 * text can later be surfaced through the DOM. These elements are never rendered, so the text is
 * retained purely for accessibility and metadata tooling.
 *
 * @param element The descriptive element to capture text for.
 * @param node The XML node containing the text content.
 */
template <typename T>
void ParseDescriptiveText(T element, const XMLNode& node) {
  std::string combined;
  for (auto child = node.firstChild(); child; child = child->nextSibling()) {
    if (child->type() == XMLNode::Type::Data || child->type() == XMLNode::Type::CData) {
      if (auto value = child->value()) {
        combined += value.value();
      }
    }
  }

  if (!combined.empty()) {
    auto& component =
        element.entityHandle().template get_or_emplace<components::DescriptiveTextComponent>();
    component.text = RcString(combined);
  }
}

template <>
std::optional<ParseDiagnostic> ParseNodeContents<SVGTitleElement>(SVGParserContext& context,
                                                                  SVGTitleElement element,
                                                                  const XMLNode& node) {
  ParseDescriptiveText(element, node);
  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseNodeContents<SVGDescElement>(SVGParserContext& context,
                                                                 SVGDescElement element,
                                                                 const XMLNode& node) {
  ParseDescriptiveText(element, node);
  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseNodeContents<SVGMetadataElement>(SVGParserContext& context,
                                                                     SVGMetadataElement element,
                                                                     const XMLNode& node) {
  ParseDescriptiveText(element, node);
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
        SourceRange range = {FileOffset::Offset(0), FileOffset::Offset(0)};
        if (auto maybeRange = context.getAttributeLocation(node, attributeName)) {
          range = *maybeRange;
        }

        context.addWarning(ParseDiagnostic::Warning(
            RcString("Unexpected namespace '" + value.value() + "'"), range));
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
/// `IsExperimental` declaration entirely rather than setting it to false - the absence of the
/// member is the default (non-experimental) state.
template <typename T>
constexpr bool IsExperimental() {
  if constexpr (requires { T::IsExperimental; }) {
    return T::IsExperimental;
  } else {
    return false;
  }
}

/// Signature of the per-tag element factory used to create and populate an element.
using CreateElementFn = ParseResult<SVGElement> (*)(SVGParserContext&, const XMLNode&,
                                                    const XMLQualifiedNameRef&);

}  // namespace

class SVGParserImpl {
private:
  SVGParserContext& context_;
  std::optional<SVGDocument> document_;
  SVGDocumentHandle documentState_;
  SVGDocument::Settings settings_;

  /// Creates an element of type \p ElementT on \p node and parses its attributes. Experimental
  /// types fall back to an unknown element unless experimental support is enabled, matching the
  /// behavior of the tag scan the factory table replaces. Element constructors are only reachable
  /// from this class, which is why the factories live here.
  template <typename ElementT>
  static ParseResult<SVGElement> createElementAndParseAttributes(
      SVGParserContext& context, const XMLNode& node, const XMLQualifiedNameRef& tagName) {
    if constexpr (IsExperimental<ElementT>()) {
      if (!context.options().enableExperimental) {
        auto element = SVGUnknownElement::CreateOn(node.entityHandle(), tagName);
        return ParseAttributes(context, element, node);
      }
    }

    auto element = ElementT::CreateOn(node.entityHandle());
    return ParseAttributes(context, element, node);
  }

  /// Builds the tag-name to factory entries for a perfect-hash lookup over the known element types.
  template <typename... Types>
  static constexpr auto makeElementFactoryEntries(entt::type_list<Types...>) {
    return std::to_array<std::pair<std::string_view, CreateElementFn>>(
        {{Types::Tag, &createElementAndParseAttributes<Types>}...});
  }

  /**
   * Look up the element factory for an SVG tag name.
   *
   * Replaces a linear scan that compared the tag against every element type's tag in turn. Lookup
   * is exact and case-sensitive, matching the `tagName.name == ElementT::Tag` comparison it
   * replaces. The lookup table's constructor rejects duplicate keys at compile time, so two
   * element types can never silently claim the same tag.
   *
   * @param tagName Tag name to look up.
   * @return Factory for the matching element type, or nullptr when no element type claims the tag.
   */
  static const CreateElementFn* lookupElementFactory(std::string_view tagName);

public:
  explicit SVGParserImpl(SVGParserContext& context, std::shared_ptr<Registry> registry,
                         SVGDocument::Settings settings)
      : context_(context),
        documentState_(std::make_shared<DocumentState>(std::move(registry))),
        settings_(std::move(settings)) {}

  std::optional<SVGDocument> document() const { return document_; }

  /**
   * Create the SVG element matching \p tagName on \p node, or an unknown element if no type
   * matches.
   *
   * @param tagName Qualified tag name to match against the known element types.
   * @param node XML node to create the element on.
   * @param isSvgNamespace Whether the tag name's prefix resolves to the SVG namespace. Resolved
   *   once by the caller rather than once per candidate type, since it is the same answer for
   *   every candidate.
   */
  ParseResult<SVGElement> createElement(const XMLQualifiedNameRef& tagName, const XMLNode& node,
                                        bool isSvgNamespace) {
    if (isSvgNamespace) {
      if (const CreateElementFn* createFn = lookupElementFactory(std::string_view(tagName.name))) {
        return (*createFn)(context_, node, tagName);
      }
    }

    auto element = SVGUnknownElement::CreateOn(node.entityHandle(), tagName);
    return ParseAttributes(context_, element, node);
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
          ss << "Ignored element <" << name << "> with an unsupported namespace. " << "Expected '"
             << context_.namespacePrefix() << "', found '" << name.namespacePrefix << "'";
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

        // The namespace check above already resolved this tag name's prefix.
        auto maybeNewElement =
            createElement(child->tagName(), child.value(), /*isSvgNamespace=*/true);
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

          document_ = SVGDocument(documentState_, std::move(settings_), child->entityHandle());

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

const CreateElementFn* SVGParserImpl::lookupElementFactory(std::string_view tagName) {
  // Defined out of line so the table is built where SVGParserImpl is a complete type; the factories
  // it stores are private members of the class.
  static DONNER_CONSTEXPR_MAP auto kElementFactories =
      makeCompileTimeMap(makeElementFactoryEntries(AllSVGElements()));
  return kElementFactories.find(tagName);
}

ParseResult<SVGDocument> SVGParser::ParseSVG(std::string_view source, ParseWarningSink& warningSink,
                                             SVGParser::Options options,
                                             SVGDocument::Settings settings) noexcept {
  if (source.size() > options.maximumInputSize) {
    return ParseDiagnostic::Error("SVG source exceeds maximum input size", FileOffset::Offset(0));
  }

  // Inject the SVG parse callback for sub-document loading, unless we're already in secure mode
  // (sub-documents cannot load their own sub-documents).
  if (!settings.svgParseCallback && settings.processingMode == ProcessingMode::DynamicInteractive) {
    settings.svgParseCallback = [](const std::vector<uint8_t>& svgContent,
                                   ParseWarningSink& warnings) -> std::optional<SVGDocumentHandle> {
      SVGDocument::Settings subSettings;
      subSettings.processingMode = ProcessingMode::SecureStatic;
      // No resource loader - secure mode sub-documents cannot load external resources.

      const std::string_view subSource(reinterpret_cast<const char*>(svgContent.data()),
                                       svgContent.size());
      auto result = SVGParser::ParseSVG(subSource, warnings, Options(), std::move(subSettings));
      if (result.hasError()) {
        warnings.add(ParseDiagnostic(result.error()));
        return std::nullopt;
      }
      return result.result().handle();
    };
  }

  xml::XMLParser::Options xmlOptions;
  xmlOptions.parseCustomEntities = true;
  xmlOptions.maximumInputSize = options.maximumInputSize;

  std::vector<uint8_t> decompressedData;
  if (source.size() >= 2 && static_cast<unsigned char>(source[0]) == 0x1F &&
      static_cast<unsigned char>(source[1]) == 0x8B) {
    auto maybeDecompressedData = Decompress::Gzip(source, options.maximumInputSize);
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

  SVGParserContext context(source, warningSink, options);
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
                                                     ParseWarningSink& warningSink,
                                                     SVGParser::Options options,
                                                     SVGDocument::Settings settings) noexcept {
  SVGParserContext context(std::string_view(), warningSink, options);
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
