#include "donner/svg/parser/SVGParser.h"

#include <array>
#include <limits>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
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
#include "donner/base/xml/XMLStringFinalizer.h"
#include "donner/base/xml/components/XMLDocumentContext.h"
#include "donner/svg/AllSVGElements.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/components/DescriptiveTextComponent.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/ParsedPayloadResourceBudget.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/parser/AttributeParser.h"
#include "donner/svg/parser/details/SVGParserContext.h"

namespace donner::svg::parser {

using xml::XMLNode;
using xml::XMLParser;
using xml::XMLQualifiedNameRef;

namespace {

constexpr std::size_t kEstimatedProjectionBytesPerChunk = 64;

bool CheckedAdd(std::size_t& total, std::size_t value) {
  if (value > std::numeric_limits<std::size_t>::max() - total) {
    return false;
  }
  total += value;
  return true;
}

bool ReserveContentProjection(SVGParserContext& context, EntityHandle handle,
                              std::size_t contentBytes, std::size_t chunkCount,
                              components::ParsedPayloadResourceBudget::Category category,
                              std::string_view description, std::size_t* reservedBytes = nullptr) {
  auto& budget = handle.registry()->ctx().get<components::ParsedPayloadResourceBudget>();
  std::size_t estimatedBytes = contentBytes;
  if (chunkCount > context.options().maximumContentProjectionChunks ||
      chunkCount > std::numeric_limits<std::size_t>::max() / kEstimatedProjectionBytesPerChunk ||
      !CheckedAdd(estimatedBytes, chunkCount * kEstimatedProjectionBytesPerChunk)) {
    budget.recordRejection();
  } else if (budget.reserve(handle.entity(), estimatedBytes, category)) {
    if (reservedBytes != nullptr) {
      *reservedBytes = estimatedBytes;
    }
    return true;
  }

  ParseDiagnostic warning;
  warning.reason = std::string(description) + " exceeds the document content-projection budget";
  context.addWarning(std::move(warning));
  return false;
}

std::optional<std::size_t> AttributePayloadBytes(const XMLNode& node) {
  std::size_t sourceBytes = 0;
  std::size_t attributeCount = 0;
  for (const XMLQualifiedNameRef& name : node.attributes()) {
    const std::optional<RcString> value = node.getAttribute(name);
    if (!value.has_value() || !CheckedAdd(sourceBytes, name.namespacePrefix.size()) ||
        !CheckedAdd(sourceBytes, name.name.size()) || !CheckedAdd(sourceBytes, value->size())) {
      return std::nullopt;
    }
    ++attributeCount;
  }
  return components::ParsedPayloadResourceBudget::estimateAttributeBytes(sourceBytes,
                                                                         attributeCount);
}

bool ReserveAttributePayload(SVGParserContext& context, EntityHandle handle, const XMLNode& node) {
  auto& budget = handle.registry()->ctx().get<components::ParsedPayloadResourceBudget>();
  const std::optional<std::size_t> bytes = AttributePayloadBytes(node);
  if (bytes.has_value() &&
      budget.reserve(handle.entity(), *bytes,
                     components::ParsedPayloadResourceBudget::Category::Attribute)) {
    return true;
  }
  if (!bytes.has_value()) {
    budget.recordRejection();
  }
  ParseDiagnostic warning;
  warning.reason = "Attributes exceed the document parsed-payload budget";
  context.addWarning(std::move(warning));
  return false;
}

template <typename T>
void ParseTextContents(SVGParserContext& context, T element, const XMLNode& node) {
  std::size_t contentBytes = 0;
  std::size_t chunkCount = 0;
  for (auto child = node.firstChild(); child; child = child->nextSibling()) {
    if (child->type() == XMLNode::Type::Data || child->type() == XMLNode::Type::CData) {
      ++chunkCount;
      if (auto value = child->value(); value && !CheckedAdd(contentBytes, value->size())) {
        auto& budget = element.entityHandle()
                           .registry()
                           ->ctx()
                           .template get<components::ParsedPayloadResourceBudget>();
        budget.recordRejection();
        return;
      }
    } else if (child->type() == XMLNode::Type::Element) {
      ++chunkCount;
    }
  }

  if (!ReserveContentProjection(context, element.entityHandle(), contentBytes, chunkCount,
                                components::ParsedPayloadResourceBudget::Category::ProjectedText,
                                "Text content")) {
    return;
  }

  std::string combined;
  combined.reserve(contentBytes);
  auto& textComponent = element.entityHandle().template get_or_emplace<components::TextComponent>();
  textComponent.textChunks.clear();
  for (auto child = node.firstChild(); child; child = child->nextSibling()) {
    if (child->type() == XMLNode::Type::Data || child->type() == XMLNode::Type::CData) {
      const RcString value = child->value().value_or(RcString(""));
      combined.append(value.data(), value.size());
      if (textComponent.textChunks.empty()) {
        textComponent.textChunks.emplace_back(value);
      } else if (textComponent.textChunks.back().empty()) {
        textComponent.textChunks.back() = value;
      } else {
        textComponent.textChunks.emplace_back(value);
      }
    } else if (child->type() == XMLNode::Type::Element) {
      if (textComponent.textChunks.empty()) {
        textComponent.textChunks.emplace_back(RcString(""));
      }
      textComponent.textChunks.emplace_back(RcString(""));
    }
  }
  textComponent.text = RcString(combined);
}

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
    std::size_t contentBytes = 0;
    std::size_t chunkCount = 0;
    for (auto child = node.firstChild(); child; child = child->nextSibling()) {
      if (child->type() == XMLNode::Type::Data || child->type() == XMLNode::Type::CData) {
        ++chunkCount;
        if (auto value = child->value(); value && !CheckedAdd(contentBytes, value->size())) {
          return ParseDiagnostic::Error("Stylesheet content size overflow", FileOffset::Offset(0));
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
    std::size_t projectedSourceBytes = 0;
    if (!ReserveContentProjection(context, element.entityHandle(), contentBytes, chunkCount,
                                  components::ParsedPayloadResourceBudget::Category::Stylesheet,
                                  "Stylesheet content", &projectedSourceBytes)) {
      return std::nullopt;
    }
    auto& payloadBudget =
        element.entityHandle().registry()->ctx().get<components::ParsedPayloadResourceBudget>();
    const std::optional<std::size_t> preflightBytes =
        components::ParsedPayloadResourceBudget::estimateStylesheetPreflightBytes(
            contentBytes, projectedSourceBytes);
    if (!preflightBytes.has_value() ||
        !payloadBudget.canReserve(element.entityHandle().entity(), *preflightBytes,
                                  components::ParsedPayloadResourceBudget::Category::Stylesheet)) {
      payloadBudget.recordRejection();
      ParseDiagnostic warning;
      warning.reason = "Stylesheet exceeds the document parsed-payload budget";
      context.addWarning(std::move(warning));
      return std::nullopt;
    }

    std::string combined;
    combined.reserve(contentBytes);
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
      }
    }
    if (!combined.empty()) {
      auto& stylesheetComponent =
          element.entityHandle().get_or_emplace<components::StylesheetComponent>();
      stylesheetComponent.parseStylesheet(std::string_view(combined), std::move(sourceMap));

      const auto& stats = stylesheetComponent.stylesheet.securityStats();
      constexpr std::size_t kEstimatedBytesPerComponentValue = 64;
      constexpr std::size_t kEstimatedBytesPerDeclaration = 128;
      constexpr std::size_t kEstimatedBytesPerRule = 128;
      std::size_t retainedBytes = projectedSourceBytes;
      const bool estimateValid =
          stats.componentValues <=
              std::numeric_limits<std::size_t>::max() / kEstimatedBytesPerComponentValue &&
          stats.declarations <=
              std::numeric_limits<std::size_t>::max() / kEstimatedBytesPerDeclaration &&
          stats.rules <= std::numeric_limits<std::size_t>::max() / kEstimatedBytesPerRule &&
          CheckedAdd(retainedBytes, stats.componentValues * kEstimatedBytesPerComponentValue) &&
          CheckedAdd(retainedBytes, stats.declarations * kEstimatedBytesPerDeclaration) &&
          CheckedAdd(retainedBytes, stats.rules * kEstimatedBytesPerRule);
      if (!estimateValid ||
          !payloadBudget.reserve(element.entityHandle().entity(), retainedBytes,
                                 components::ParsedPayloadResourceBudget::Category::Stylesheet)) {
        stylesheetComponent = components::StylesheetComponent{};
        ParseDiagnostic warning;
        warning.reason = "Stylesheet exceeds the document parsed-payload budget";
        context.addWarning(std::move(warning));
      }
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
  ParseTextContents(context, element, node);
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
  ParseTextContents(context, element, node);
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
  ParseTextContents(context, element, node);
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
  ParseTextContents(context, element, node);
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
void ParseDescriptiveText(SVGParserContext& context, T element, const XMLNode& node) {
  std::size_t contentBytes = 0;
  std::size_t chunkCount = 0;
  for (auto child = node.firstChild(); child; child = child->nextSibling()) {
    if (child->type() == XMLNode::Type::Data || child->type() == XMLNode::Type::CData) {
      ++chunkCount;
      if (auto value = child->value(); value && !CheckedAdd(contentBytes, value->size())) {
        element.entityHandle()
            .registry()
            ->ctx()
            .template get<components::ParsedPayloadResourceBudget>()
            .recordRejection();
        return;
      }
    }
  }

  if (!ReserveContentProjection(context, element.entityHandle(), contentBytes, chunkCount,
                                components::ParsedPayloadResourceBudget::Category::ProjectedText,
                                "Descriptive content")) {
    return;
  }

  std::string combined;
  combined.reserve(contentBytes);
  for (auto child = node.firstChild(); child; child = child->nextSibling()) {
    if (child->type() == XMLNode::Type::Data || child->type() == XMLNode::Type::CData) {
      if (auto value = child->value()) {
        combined.append(value->data(), value->size());
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
  ParseDescriptiveText(context, element, node);
  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseNodeContents<SVGDescElement>(SVGParserContext& context,
                                                                 SVGDescElement element,
                                                                 const XMLNode& node) {
  ParseDescriptiveText(context, element, node);
  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseNodeContents<SVGMetadataElement>(SVGParserContext& context,
                                                                     SVGMetadataElement element,
                                                                     const XMLNode& node) {
  ParseDescriptiveText(context, element, node);
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
  if (!ReserveAttributePayload(context, element.entityHandle(), node)) {
    return std::move(element);
  }
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
  std::size_t visitedTreeNodes_ = 0;

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
   * replaces.
   *
   * Two element types claiming the same tag is a build error wherever the lookup table is built as
   * a constant expression, which is every configuration except the one that degrades the table to
   * runtime initialization to stay inside a lower constexpr step budget. There the duplicate
   * instead makes the map fall back to a linear scan that returns the first matching key, which is
   * the entry the replaced scan would also have chosen.
   *
   * @param tagName Tag name to look up.
   * @return Factory for the matching element type, or nullptr when no element type claims the tag.
   */
  static const CreateElementFn* lookupElementFactory(std::string_view tagName);

public:
  explicit SVGParserImpl(SVGParserContext& context, std::shared_ptr<Registry> sharedRegistry,
                         SVGDocument::Settings settings)
      : context_(context),
        documentState_(std::make_shared<DocumentState>(std::move(sharedRegistry))),
        settings_(std::move(settings)) {
    Registry& registry = documentState_->registry();
    if (!settings_.resourceFamilyBudget) {
      settings_.resourceFamilyBudget = std::make_shared<components::DocumentResourceFamilyBudget>(
          components::DocumentResourceFamilyBudget::Limits{
              .parsedPayloadBytes = context.options().maximumParsedPayloadSize,
          });
    }
    if (!registry.ctx().contains<components::DocumentResourceFamilyContext>()) {
      registry.ctx().emplace<components::DocumentResourceFamilyContext>(
          settings_.resourceFamilyBudget);
    }
    if (!registry.ctx().contains<components::ParsedPayloadResourceBudget>()) {
      registry.ctx().emplace<components::ParsedPayloadResourceBudget>(
          components::ParsedPayloadResourceBudget::Limits{
              .maximumRetainedBytes = context.options().maximumParsedPayloadSize,
          },
          settings_.resourceFamilyBudget);
    }
  }

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
                                              const XMLNode& rootNode, std::size_t parentDepth) {
    bool foundRootSvg = false;

    for (auto child = rootNode.firstChild(); child;) {
      if (visitedTreeNodes_ >= context_.options().maximumTreeNodes) {
        ParseDiagnostic err;
        err.reason = "Maximum SVG conversion tree-node count exceeded";
        if (auto sourceOffset = child->sourceStartOffset()) {
          err.range.start = *sourceOffset;
        }
        return err;
      }
      ++visitedTreeNodes_;

      const XMLQualifiedNameRef name = child->tagName();

      const XMLNode::Type type = child->type();
      if (type != XMLNode::Type::Element) {
        // Remove the unknown element from the tree.
        XMLNode nodeToRemove = child.value();
        child = child->nextSibling();
        nodeToRemove.remove();
        continue;
      }

      const std::size_t childDepth = parentDepth + 1;
      if (childDepth > context_.options().maximumTreeDepth) {
        ParseDiagnostic err;
        err.reason = "Maximum SVG conversion element depth exceeded";
        if (auto sourceOffset = child->sourceStartOffset()) {
          err.range.start = *sourceOffset;
        }
        return err;
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

        if (auto error = walkChildren(maybeNewElement.result(), child.value(), childDepth)) {
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
          documentState_->registry()
              .ctx()
              .get<components::SVGDocumentContext>()
              .maximumContentProjectionChunks = context_.options().maximumContentProjectionChunks;

          auto maybeSvgElement = ParseAttributes(context_, document_->svgElement(), child.value());
          if (maybeSvgElement.hasError()) {
            return std::move(maybeSvgElement.error());
          }

          foundRootSvg = true;
          if (auto error = walkChildren(maybeSvgElement.result(), child.value(), childDepth)) {
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

namespace {

SVGDocument::Settings PrepareDocumentSettings(const SVGParser::Options& options,
                                              SVGDocument::Settings settings) {
  if (!settings.resourceFamilyBudget) {
    settings.resourceFamilyBudget = std::make_shared<components::DocumentResourceFamilyBudget>(
        components::DocumentResourceFamilyBudget::Limits{
            .parsedPayloadBytes = options.maximumParsedPayloadSize,
        });
  }
  if (settings.svgParseCallback || settings.processingMode != ProcessingMode::DynamicInteractive) {
    return settings;
  }

  const std::shared_ptr<components::DocumentResourceFamilyBudget> resourceFamily =
      settings.resourceFamilyBudget;
  settings.svgParseCallback = [resourceFamily](
                                  const std::vector<uint8_t>& svgContent,
                                  ParseWarningSink& warnings) -> std::optional<SVGDocumentHandle> {
    SVGDocument::Settings subSettings;
    subSettings.processingMode = ProcessingMode::SecureStatic;
    subSettings.resourceFamilyBudget = resourceFamily;

    const std::string_view subSource(reinterpret_cast<const char*>(svgContent.data()),
                                     svgContent.size());
    SVGParser::Options subOptions;
    subOptions.retainSource = false;
    auto result = SVGParser::ParseSVG(subSource, warnings, subOptions, std::move(subSettings));
    if (result.hasError()) {
      warnings.add(ParseDiagnostic(result.error()));
      return std::nullopt;
    }
    return result.result().handle();
  };
  return settings;
}

ParseResult<xml::XMLDocument> ParseXmlDocument(std::string_view source,
                                               const SVGParser::Options& options) {
  xml::XMLParser::Options xmlOptions;
  xmlOptions.parseCustomEntities = true;
  xmlOptions.maximumInputSize = options.maximumInputSize;
  xmlOptions.maxElements = options.maximumTreeNodes;
  xmlOptions.maxNestingDepth = static_cast<int>(std::min(
      options.maximumTreeDepth, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  xmlOptions.stringStorageMode = options.retainSource ? XMLParser::StringStorageMode::SourceRetained
                                                      : XMLParser::StringStorageMode::Compact;

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
  xml::XMLDocument document(maybeDocument.result());
  document.setSourceEditTreeLimits(options.maximumTreeNodes, options.maximumTreeDepth);
  return document;
}

}  // namespace

ParseResult<SVGDocument> SVGParser::ParseSVG(std::string_view source, ParseWarningSink& warningSink,
                                             SVGParser::Options options,
                                             SVGDocument::Settings settings) noexcept {
  if (source.size() > options.maximumInputSize) {
    return ParseDiagnostic::Error("SVG source exceeds maximum input size", FileOffset::Offset(0));
  }

  settings = PrepareDocumentSettings(options, std::move(settings));
  auto maybeXmlDocument = ParseXmlDocument(source, options);
  if (maybeXmlDocument.hasError()) {
    return std::move(maybeXmlDocument.error());
  }
  xml::XMLDocument xmlDocument(maybeXmlDocument.result());
  SVGParserContext context(xmlDocument.source(), warningSink, options);
  SVGParserImpl parser(context, xmlDocument.sharedRegistry(), std::move(settings));
  if (auto error = parser.walkChildren(std::nullopt, xmlDocument.root(), 0)) {
    return std::move(error.value());
  }

  if (auto maybeDocument = parser.document()) {
    SVGDocument document = std::move(maybeDocument.value());
    if (!options.retainSource) {
      document.unsafeRegistry()
          .ctx()
          .get<xml::components::XMLDocumentContext>()
          .sourceStore.reset();
    }
    return document;
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
  xmlDocument.setSourceEditTreeLimits(options.maximumTreeNodes, options.maximumTreeDepth);
  if (!options.retainSource) {
    xml::FinalizeXMLDocumentStrings(xmlDocument);
  }
  SVGParserContext context(std::string_view(), warningSink, options);
  SVGParserImpl parser(context, xmlDocument.sharedRegistry(), std::move(settings));
  if (auto error = parser.walkChildren(std::nullopt, xmlDocument.root(), 0)) {
    return std::move(error.value());
  }

  if (auto maybeDocument = parser.document()) {
    SVGDocument document = std::move(maybeDocument.value());
    if (!options.retainSource) {
      document.unsafeRegistry()
          .ctx()
          .get<xml::components::XMLDocumentContext>()
          .sourceStore.reset();
    }
    return document;
  } else {
    ParseDiagnostic err;
    err.reason = "No SVG element found in document";
    err.range.start = FileOffset::Offset(0);
    return err;
  }
}

}  // namespace donner::svg::parser
