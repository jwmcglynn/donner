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

#include "donner/base/ParseWarningSink.h"
#include "donner/base/RcString.h"
#include "donner/base/encoding/Decompress.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/svg/AllSVGElements.h"
#include "donner/svg/ElementFactory.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/components/DescriptiveTextComponent.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/ParsedPayloadResourceBudget.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/style/SelectorContent.h"
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

void RecordDiscardedTextForParent(const std::optional<SVGElement>& element, const XMLNode& node) {
  if (element) {
    components::RecordDiscardedSelectorText(element->unsafeEntityHandle(), node);
  }
}

struct ProjectedTextSize {
  std::size_t contentBytes = 0;
  std::size_t chunkCount = 0;
};

/// Measure direct text children before reserving their projected storage.
/// @param element Element whose payload budget records an overflow rejection.
/// @param node XML parent containing the source children.
/// @param includeElementChunks Whether nested elements need empty text-chunk markers.
/// @return Content bytes and chunk count, or nullopt after recording an overflow rejection.
std::optional<ProjectedTextSize> MeasureProjectedText(SVGElement element, const XMLNode& node,
                                                      bool includeElementChunks) {
  ProjectedTextSize size;
  for (auto child = node.firstChild(); child; child = child->nextSibling()) {
    if (child->type() == XMLNode::Type::Data || child->type() == XMLNode::Type::CData) {
      ++size.chunkCount;
      if (auto value = child->value(); value && !CheckedAdd(size.contentBytes, value->size())) {
        auto& budget =
            element.entityHandle().registry()->ctx().get<components::ParsedPayloadResourceBudget>();
        budget.recordRejection();
        return std::nullopt;
      }
    } else if (includeElementChunks && child->type() == XMLNode::Type::Element) {
      ++size.chunkCount;
    }
  }
  return size;
}

void ParseTextContents(SVGParserContext& context, SVGElement element, const XMLNode& node) {
  const std::optional<ProjectedTextSize> size = MeasureProjectedText(element, node, true);
  if (!size) {
    return;
  }
  if (!ReserveContentProjection(
          context, element.entityHandle(), size->contentBytes, size->chunkCount,
          components::ParsedPayloadResourceBudget::Category::ProjectedText, "Text content")) {
    return;
  }

  std::string combined;
  combined.reserve(size->contentBytes);
  auto& textComponent = element.entityHandle().get_or_emplace<components::TextComponent>();
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

SourceRange MapStylesheetWarning(SVGParserContext& context,
                                 const components::StylesheetSourceMap& sourceMap,
                                 const SourceRange& range) {
  std::optional<SourceRange> mapped;
  if (range.start == range.end) {
    if (const auto point = sourceMap.mapPointToDocumentSource(range.start)) {
      mapped = SourceRange{*point, *point};
    }
  } else {
    mapped = sourceMap.mapToDocumentSource(range);
  }
  return mapped ? context.documentSourceRange(*mapped) : range;
}

void ParseStylesheetWithWarnings(SVGParserContext& context,
                                 components::StylesheetComponent& component,
                                 std::string_view source,
                                 components::StylesheetSourceMap sourceMap) {
  ParseWarningSink warnings =
      context.warningsEnabled() ? ParseWarningSink() : ParseWarningSink::Disabled();
  component.parseStylesheet(source, std::move(sourceMap), warnings);
  warnings.mapSourceRanges([&](const SourceRange& range) {
    return MapStylesheetWarning(context, component.sourceMap, range);
  });
  context.addRecoverableWarnings(std::move(warnings));
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
    sourceMap.setCssLength(combined.size());
    if (!combined.empty()) {
      auto& stylesheetComponent =
          element.entityHandle().get_or_emplace<components::StylesheetComponent>();
      ParseStylesheetWithWarnings(context, stylesheetComponent, std::string_view(combined),
                                  std::move(sourceMap));

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
void ParseDescriptiveText(SVGParserContext& context, SVGElement element, const XMLNode& node) {
  const std::optional<ProjectedTextSize> size = MeasureProjectedText(element, node, false);
  if (!size) {
    return;
  }
  if (!ReserveContentProjection(context, element.entityHandle(), size->contentBytes,
                                size->chunkCount,
                                components::ParsedPayloadResourceBudget::Category::ProjectedText,
                                "Descriptive content")) {
    return;
  }

  std::string combined;
  combined.reserve(size->contentBytes);
  for (auto child = node.firstChild(); child; child = child->nextSibling()) {
    if (child->type() == XMLNode::Type::Data || child->type() == XMLNode::Type::CData) {
      if (auto value = child->value()) {
        combined.append(value->data(), value->size());
      }
    }
  }

  if (!combined.empty()) {
    auto& component = element.entityHandle().get_or_emplace<components::DescriptiveTextComponent>();
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

/// Whether \p node has a direct text or CDATA child.
bool HasTextChild(const XMLNode& node) {
  for (auto child = node.firstChild(); child; child = child->nextSibling()) {
    if (child->type() == XMLNode::Type::Data || child->type() == XMLNode::Type::CData) {
      return true;
    }
  }
  return false;
}

/**
 * Parse the text of a descriptive element after the parse, when the element has no text children
 * left. The SVG parser removes the text children of every element it converts, and the XML parser
 * keeps an element's last plain-text chunk as its value, which a later source edit of that text
 * updates. That value is therefore all the text such an element still has, whether it is a parsed
 * `<title>` whose text is edited or a retained foreign `<x:title>` that an `xmlns` edit moves into
 * the SVG namespace. As for projected `<text>`, only the last plain-text chunk survives: CDATA
 * content is lost, and text split by a comment or CDATA section keeps only its last plain-text
 * chunk.
 *
 * @param context The parser context.
 * @param element The descriptive element.
 * @param node The element's XML node.
 */
void ParseRetainedDescriptiveText(SVGParserContext& context, const SVGElement& element,
                                  const XMLNode& node) {
  const RcString value = node.value().value_or(RcString(""));
  if (!ReserveContentProjection(context, element.entityHandle(), value.size(),
                                value.empty() ? 0 : 1,
                                components::ParsedPayloadResourceBudget::Category::ProjectedText,
                                "Descriptive content") ||
      value.empty()) {
    return;
  }
  element.entityHandle().get_or_emplace<components::DescriptiveTextComponent>().text = value;
}

/// Parse the contents that only the parser projects, after the parse: the text of a `<title>`,
/// `<desc>` or `<metadata>` element.
void ParseDescriptiveContents(SVGParserContext& context, const SVGElement& element,
                              const XMLNode& node) {
  switch (element.type()) {
    case ElementType::Title:
    case ElementType::Desc:
    case ElementType::Metadata:
      if (HasTextChild(node)) {
        ParseDescriptiveText(context, element, node);
      } else {
        ParseRetainedDescriptiveText(context, element, node);
      }
      break;
    default: break;
  }
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

/// Parse each of \p node's attributes onto \p element, skipping attributes in an unsupported
/// namespace.
std::optional<ParseDiagnostic> ParseAttributeList(SVGParserContext& context, SVGElement element,
                                                  const XMLNode& node) {
  for (const XMLQualifiedNameRef& attributeName : node.attributes()) {
    const RcString value = node.getAttribute(attributeName).value();

    if (!attributeName.namespacePrefix.empty() && attributeName.namespacePrefix != "xmlns" &&
        attributeName.namespacePrefix != "xlink" &&
        node.getNamespaceUri(attributeName.namespacePrefix) != "http://www.w3.org/2000/svg") {
      if (context.warningsEnabled()) {
        ParseDiagnostic err;
        std::ostringstream ss;
        ss << "Ignored attribute '" << attributeName << "' with an unsupported namespace";
        err.reason = ss.str();
        if (auto maybeRange = context.getAttributeLocation(element, attributeName)) {
          err.range.start = maybeRange->start;
        }

        context.addWarning(std::move(err));
      }
      continue;
    }

    if (auto error =
            AttributeParser::ParseAndSetAttribute(context, element, attributeName, value)) {
      return error;
    }
  }

  return std::nullopt;
}

using NodeContentsCallback = std::optional<ParseDiagnostic> (*)(SVGParserContext&, const void*,
                                                                const XMLNode&);

/// Parse attributes before invoking the original element type's contents parser.
/// @param context Parser context and resource limits.
/// @param element Base handle to the element being parsed.
/// @param node XML node supplying attributes and contents.
/// @param typedElement Original typed element, borrowed only until this function returns.
/// @param parseContents Synchronous typed contents callback; never stored.
/// @return The parsed element or the first diagnostic.
ParseResult<SVGElement> ParseAttributesAndContents(SVGParserContext& context, SVGElement element,
                                                   const XMLNode& node, const void* typedElement,
                                                   NodeContentsCallback parseContents) {
  if (!ReserveAttributePayload(context, element.entityHandle(), node)) {
    return std::move(element);
  }
  if (auto error = ParseAttributeList(context, element, node)) {
    return std::move(error.value());
  }

  if (auto error = parseContents(context, typedElement, node)) {
    return std::move(error.value());
  }

  return std::move(element);
}

template <typename T>
ParseResult<SVGElement> ParseAttributes(SVGParserContext& context, T element, const XMLNode& node) {
  return ParseAttributesAndContents(
      context, element, node, &element,
      [](SVGParserContext& parserContext, const void* typedElement, const XMLNode& sourceNode) {
        return ParseNodeContents(parserContext, *static_cast<const T*>(typedElement), sourceNode);
      });
}

/**
 * Parse the contents that only the parser projects, as \ref ParseDescriptiveContents does, and
 * report whether the document's content-projection budget refused them. Every refusal records a
 * rejection on the budget before anything is stored, so a changed rejection count is exact.
 *
 * @param context The parser context.
 * @param element The element.
 * @param node The element's XML node.
 * @return An error if the budget refused the contents.
 */
std::optional<ParseDiagnostic> ParseDescriptiveContentsWithinBudget(SVGParserContext& context,
                                                                    const SVGElement& element,
                                                                    const XMLNode& node) {
  const auto& budget =
      element.entityHandle().registry()->ctx().get<components::ParsedPayloadResourceBudget>();
  const std::size_t rejectionsBefore = budget.securityStats().rejectedReservations;
  ParseDescriptiveContents(context, element, node);
  if (budget.securityStats().rejectedReservations == rejectionsBefore) {
    return std::nullopt;
  }
  return ParseDiagnostic::Error(
      "Descriptive content exceeds the document content-projection budget",
      node.getNodeLocation().value_or(SourceRange{FileOffset::Offset(0), FileOffset::Offset(0)}));
}

/**
 * Parse an element that an incremental source edit has just built, as a full parse would: its
 * attributes, and the text of a `<title>`, `<desc>` or `<metadata>` element. The parser installs
 * this as \ref components::SVGDocumentContext::parseBuiltElement.
 *
 * @param options Options of the parse that built the document.
 * @param source Current document source.
 * @param element The built element.
 * @param node The element's XML node.
 * @return The first attribute error, or an error if the budget refuses the element's text.
 */
std::optional<ParseDiagnostic> ParseBuiltElement(const SVGParser::Options& options,
                                                 std::string_view source, const SVGElement& element,
                                                 const XMLNode& node) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  SVGParserContext context(source, warnings, options);
  if (auto error = ParseAttributeList(context, element, node)) {
    return error;
  }
  return ParseDescriptiveContentsWithinBudget(context, element, node);
}

/**
 * Parse the contents of an element whose text a source edit has changed, as a full parse would:
 * the new text of a `<title>`, `<desc>` or `<metadata>` element replaces its old text. If the
 * document's content-projection budget rejects the new text, the element keeps its old text and
 * reservation, as projected `<text>` does, and the edit reports an error. The parser installs this
 * as \ref components::SVGDocumentContext::parseEditedElementContents.
 *
 * @param options Options of the parse that built the document.
 * @param source Current document source.
 * @param element The edited element.
 * @param node The element's XML node.
 * @return An error if the budget rejects the new text.
 */
std::optional<ParseDiagnostic> ParseEditedElementContents(const SVGParser::Options& options,
                                                          std::string_view source,
                                                          const SVGElement& element,
                                                          const XMLNode& node) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  SVGParserContext context(source, warnings, options);
  const EntityHandle handle = element.entityHandle();
  std::optional<components::DescriptiveTextComponent> previous;
  if (auto* component = handle.try_get<components::DescriptiveTextComponent>()) {
    previous = std::move(*component);
    handle.remove<components::DescriptiveTextComponent>();
  }

  std::optional<ParseDiagnostic> error =
      ParseDescriptiveContentsWithinBudget(context, element, node);
  // A refusal stores no new text, and a refused reservation leaves the old one in place, so
  // restoring the old text keeps the two consistent.
  if (error.has_value() && previous.has_value()) {
    handle.emplace_or_replace<components::DescriptiveTextComponent>(std::move(*previous));
  }
  return error;
}

}  // namespace

class SVGParserImpl {
private:
  SVGParserContext& context_;
  std::optional<SVGDocument> document_;
  SVGDocumentHandle documentState_;
  SVGDocument::Settings settings_;
  std::size_t visitedTreeNodes_ = 0;

  /// Nesting depth of retained foreign-namespace subtrees currently being converted; nonzero
  /// means the unsupported-namespace warning was already reported at the subtree's top.
  /// Counted rather than a flag so a foreign element nested in a foreign element restores the
  /// enclosing state instead of clearing it.
  int foreignSubtreeDepth_ = 0;

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
   * Create the SVG element matching \p tagName on \p node, or an unknown element if no enabled
   * type matches, and parse its attributes and contents.
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
      std::optional<ParseResult<SVGElement>> typedElement;
      if (svg::details::ElementFactory::VisitType(
              std::string_view(tagName.name), context_.options().enableExperimental,
              [&]<typename ElementT>() {
                typedElement = ParseAttributes(
                    context_, svg::details::ElementFactory::CreateOn<ElementT>(node.entityHandle()),
                    node);
              })) {
        return std::move(*typedElement);
      }
    }

    auto element = svg::details::ElementFactory::CreateUnknownOn(node.entityHandle(), tagName);
    node.entityHandle().get<components::ElementTypeComponent>().setInSvgNamespace(isSvgNamespace);
    return ParseAttributes(context_, element, node);
  }

  /// Marks the conversion as being inside a retained foreign-namespace subtree for its lifetime.
  /// Unconditional so early returns unwind it, and counted so nested foreign elements restore
  /// the enclosing state rather than clearing it.
  class ForeignSubtreeScope {
  public:
    ForeignSubtreeScope(SVGParserImpl* parser, bool foreign) : parser_(parser), foreign_(foreign) {
      parser_->foreignSubtreeDepth_ += static_cast<int>(foreign_);
    }
    ~ForeignSubtreeScope() { parser_->foreignSubtreeDepth_ -= static_cast<int>(foreign_); }

    ForeignSubtreeScope(const ForeignSubtreeScope&) = delete;
    ForeignSubtreeScope& operator=(const ForeignSubtreeScope&) = delete;

  private:
    SVGParserImpl* parser_;
    bool foreign_;
  };

  /**
   * Resolve whether \p child is in the SVG namespace, reporting the unsupported-namespace
   * warning once at the top of a retained foreign subtree rather than once per descendant.
   *
   * @param child XML node being converted.
   * @param name Qualified tag name of \p child, already read by the caller.
   * @return True when the tag name's prefix resolves to the SVG namespace.
   */
  bool resolveSvgNamespace(const XMLNode& child, const XMLQualifiedNameRef& name) {
    const std::optional<RcString> maybeUri = child.getNamespaceUri(name.namespacePrefix);
    const bool isSvgNamespace = maybeUri == "http://www.w3.org/2000/svg";
    if (isSvgNamespace || foreignSubtreeDepth_ > 0) {
      return isSvgNamespace;
    }

    ParseDiagnostic err;
    std::ostringstream ss;
    ss << "Retaining element <" << name << "> with an unsupported namespace as unknown. "
       << "Expected '" << context_.namespacePrefix() << "', found '" << name.namespacePrefix << "'";
    err.reason = ss.str();
    if (auto sourceOffset = child.sourceStartOffset()) {
      err.range.start = sourceOffset.value();
    }
    context_.addWarning(std::move(err));
    return false;
  }

  /**
   * Convert \p rootNode's children into SVG elements.
   *
   * @param element Parent SVG element, or std::nullopt while looking for the root `<svg>`.
   * @param rootNode XML node whose children are converted.
   * @param parentDepth Element depth of \p rootNode, checked against the depth limit.
   */
  std::optional<ParseDiagnostic> walkChildren(std::optional<SVGElement> element,
                                              const XMLNode& rootNode, std::size_t parentDepth) {
    bool foundRootSvg = false;
    RecordDiscardedTextForParent(element, rootNode);

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
        nodeToRemove.removeForProjection();
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

        // Foreign-namespace elements are retained as unknown elements rather than detached, so
        // the shared XML tree stays complete for whole-tree consumers and their SVG-namespace
        // children still project. The resolved answer feeds both the factory lookup and the
        // non-rendering marking inside createElement.
        const bool isSvgNamespace = resolveSvgNamespace(child.value(), name);
        const ForeignSubtreeScope foreignScope(this, !isSvgNamespace);

        auto maybeNewElement = createElement(child->tagName(), child.value(), isSvgNamespace);
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
          auto& documentContext =
              documentState_->registry().ctx().get<components::SVGDocumentContext>();
          documentContext.maximumContentProjectionChunks =
              context_.options().maximumContentProjectionChunks;
          documentContext.experimentalElementsEnabled = context_.options().enableExperimental;
          documentContext.parseBuiltElement = [options = context_.options()](
                                                  std::string_view source,
                                                  const SVGElement& element, const XMLNode& node) {
            return ParseBuiltElement(options, source, element, node);
          };
          documentContext.parseEditedElementContents =
              [options = context_.options()](std::string_view source, const SVGElement& element,
                                             const XMLNode& node) {
                return ParseEditedElementContents(options, source, element, node);
              };

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

namespace {

SVGDocument::Settings PrepareDocumentSettings(const SVGParser::Options& options,
                                              SVGDocument::Settings settings) {
  if (!settings.resourceFamilyBudget) {
    settings.resourceFamilyBudget = std::make_shared<components::DocumentResourceFamilyBudget>(
        components::DocumentResourceFamilyBudget::Limits{
            .parsedPayloadBytes = options.maximumParsedPayloadSize,
        });
  }
  if (settings.svgParseCallback) {
    return settings;
  }

  const std::shared_ptr<components::DocumentResourceFamilyBudget> resourceFamily =
      settings.resourceFamilyBudget;
  SVGParser::Options subOptions = options;
  // Image resources are independent documents, even when the parent was parsed inline.
  subOptions.parseAsInlineSVG = false;
  settings.svgParseCallback = [resourceFamily, subOptions](
                                  const std::vector<uint8_t>& svgContent,
                                  ParseWarningSink& warnings) -> std::optional<SVGDocumentHandle> {
    SVGDocument::Settings subSettings;
    subSettings.processingMode = ProcessingMode::SecureStatic;
    subSettings.resourceFamilyBudget = resourceFamily;

    const std::string_view subSource(reinterpret_cast<const char*>(svgContent.data()),
                                     svgContent.size());
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
  xmlDocument.setSourceEditTreeLimits(options.maximumTreeNodes, options.maximumTreeDepth);
  SVGParserContext context(std::string_view(), warningSink, options);
  SVGParserImpl parser(context, xmlDocument.sharedRegistry(), std::move(settings));
  if (auto error = parser.walkChildren(std::nullopt, xmlDocument.root(), 0)) {
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
