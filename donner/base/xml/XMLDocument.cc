#include "donner/base/xml/XMLDocument.h"

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "donner/base/xml/XMLEscape.h"
#include "donner/base/xml/XMLIncrementalParser.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/base/xml/components/TreeMutationContext.h"
#include "donner/base/xml/components/XMLDocumentContext.h"
#include "donner/base/xml/components/XMLNamespaceContext.h"
#include "donner/base/xml/components/XMLValueComponent.h"

namespace donner::xml {

using components::XMLDocumentContext;
using components::XMLNamespaceContext;

namespace internal {

static_assert(XMLDocumentContext::kDefaultMaximumSourceEditTreeNodes ==
              XMLParser::Options::kDefaultMaximumElements);
static_assert(XMLDocumentContext::kDefaultMaximumSourceEditTreeDepth ==
              XMLParser::Options::kDefaultMaximumNestingDepth);
static_assert(XMLDocumentContext::kDefaultMaximumSourceEditTotalAttributes ==
              XMLParser::Options::kDefaultMaximumTotalAttributes);

struct SourceEditRange {
  std::size_t start = 0;
  std::size_t end = 0;
};

struct AttributeValueEdit {
  XMLNode node;
  XMLQualifiedName name;
  SourceRange attributeLocation;
  std::size_t valueStart = 0;
  std::size_t valueEnd = 0;
  char quote = '"';
};

struct OpeningTagEdit {
  XMLNode node;
  std::size_t tagStart = 0;
  std::size_t tagEnd = 0;
};

enum class TextNodeEditKind : uint8_t {
  ParsedPcdata,
  RawTextLikeNode,
};

struct TextNodeEdit {
  XMLNode node;
  TextNodeEditKind kind = TextNodeEditKind::ParsedPcdata;
  bool elementTextContent = false;
};

struct ElementSubtreeEdit {
  XMLNode node;
};

struct SourceEditClassification {
  std::optional<AttributeValueEdit> attribute;
  std::optional<OpeningTagEdit> openingTag;
  std::optional<TextNodeEdit> textNode;
  std::optional<ElementSubtreeEdit> elementSubtree;
  ReparseScope scope = ReparseScope::Document;
};

using AttributeMap = std::map<XMLQualifiedName, RcString>;

bool IsXmlWhitespace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

void AppendQualifiedName(std::string& out, const XMLQualifiedNameRef& name) {
  if (!name.namespacePrefix.empty()) {
    out.append(name.namespacePrefix);
    out.push_back(':');
  }
  out.append(name.name);
}

bool ContainsOffset(const SourceRange& range, std::size_t offset) {
  if (!range.start.offset.has_value() || !range.end.offset.has_value()) {
    return false;
  }

  return *range.start.offset <= offset && offset < *range.end.offset;
}

std::optional<std::size_t> FindOpeningTagEnd(std::string_view source, std::size_t tagStart) {
  if (tagStart >= source.size() || source[tagStart] != '<') {
    return std::nullopt;
  }

  bool inSingleQuote = false;
  bool inDoubleQuote = false;
  for (std::size_t pos = tagStart + 1; pos < source.size(); ++pos) {
    const char ch = source[pos];
    if (!inSingleQuote && !inDoubleQuote) {
      if (ch == '\'') {
        inSingleQuote = true;
      } else if (ch == '"') {
        inDoubleQuote = true;
      } else if (ch == '>') {
        return pos + 1;
      } else if (ch == '<') {
        return std::nullopt;
      }
    } else if (inSingleQuote && ch == '\'') {
      inSingleQuote = false;
    } else if (inDoubleQuote && ch == '"') {
      inDoubleQuote = false;
    }
  }

  return std::nullopt;
}

std::optional<std::size_t> FindDirtyOpeningTagEnd(std::string_view source, std::size_t tagStart,
                                                  std::size_t nodeEnd) {
  if (tagStart >= source.size() || source[tagStart] != '<' || nodeEnd <= tagStart) {
    return std::nullopt;
  }

  const std::size_t scanEnd = nodeEnd < source.size() ? nodeEnd : source.size();
  for (std::size_t pos = tagStart + 1; pos < scanEnd; ++pos) {
    if (source[pos] == '>') {
      return pos + 1;
    }
  }

  return std::nullopt;
}

std::optional<std::size_t> FindClosingTagStart(std::string_view source, std::size_t nodeEnd) {
  if (nodeEnd == 0 || nodeEnd > source.size()) {
    return std::nullopt;
  }

  const std::size_t closingTagStart = source.rfind("</", nodeEnd - 1);
  if (closingTagStart == std::string_view::npos || closingTagStart >= nodeEnd) {
    return std::nullopt;
  }

  return closingTagStart;
}

std::optional<SourceEditRange> ResolveEditRange(const SourceRange& range, std::string_view source) {
  if (!range.start.offset.has_value() || !range.end.offset.has_value() ||
      *range.end.offset < *range.start.offset || *range.end.offset > source.size()) {
    return std::nullopt;
  }

  return SourceEditRange{
      .start = *range.start.offset,
      .end = *range.end.offset,
  };
}

std::optional<SourceEditRange> ResolveNodeValueRange(const XMLNode& node, std::string_view source) {
  std::optional<SourceRange> valueLocation = node.getValueLocation();
  if (!valueLocation.has_value()) {
    return std::nullopt;
  }

  return ResolveEditRange(*valueLocation, source);
}

bool ContainsEditRange(SourceEditRange container, SourceEditRange edit) {
  return container.start <= edit.start && edit.end <= container.end;
}

SourceRange ToSourceRange(SourceEditRange range) {
  return SourceRange{
      FileOffset::Offset(range.start),
      FileOffset::Offset(range.end),
  };
}

ParseDiagnostic RebaseDiagnosticToDirtyRange(ParseDiagnostic diagnostic, SourceEditRange range) {
  diagnostic.range = ToSourceRange(range);
  return diagnostic;
}

bool IsRawTextLikeNode(XMLNode::Type type) {
  return type == XMLNode::Type::CData || type == XMLNode::Type::Comment ||
         type == XMLNode::Type::ProcessingInstruction;
}

std::optional<AttributeValueEdit> GetAttributeValueSpan(const XMLNode& node,
                                                        const XMLQualifiedName& name,
                                                        const SourceRange& attributeLocation,
                                                        std::string_view source) {
  std::optional<SourceEditRange> attributeRange = ResolveEditRange(attributeLocation, source);
  if (!attributeRange.has_value() || attributeRange->end <= attributeRange->start) {
    return std::nullopt;
  }

  const std::string_view attributeSource =
      source.substr(attributeRange->start, attributeRange->end - attributeRange->start);
  const std::size_t equalsOffset = attributeSource.find('=');
  if (equalsOffset == std::string_view::npos) {
    return std::nullopt;
  }

  std::size_t quoteOffset = equalsOffset + 1;
  while (quoteOffset < attributeSource.size() && IsXmlWhitespace(attributeSource[quoteOffset])) {
    ++quoteOffset;
  }

  if (quoteOffset >= attributeSource.size()) {
    return std::nullopt;
  }

  const char quote = attributeSource[quoteOffset];
  if ((quote != '"' && quote != '\'') || attributeSource.back() != quote) {
    return std::nullopt;
  }

  return AttributeValueEdit{
      .node = node,
      .name = name,
      .attributeLocation = attributeLocation,
      .valueStart = attributeRange->start + quoteOffset + 1,
      .valueEnd = attributeRange->end - 1,
      .quote = quote,
  };
}

struct SourceNodeInterval {
  XMLNode node;
  SourceRange range;
  std::size_t depth = 0;
};

using SourceIntervalIndex = std::vector<SourceNodeInterval>;

void BuildSourceIntervalIndex(const XMLNode& node, std::size_t depth, SourceIntervalIndex& index) {
  std::optional<SourceRange> nodeLocation = node.getNodeLocation();
  if (nodeLocation.has_value() && nodeLocation->start.offset.has_value() &&
      nodeLocation->end.offset.has_value()) {
    index.push_back(SourceNodeInterval{
        .node = node,
        .range = *nodeLocation,
        .depth = depth,
    });
  }

  for (std::optional<XMLNode> child = node.firstChild(); child.has_value();
       child = child->nextSibling()) {
    BuildSourceIntervalIndex(*child, depth + 1, index);
  }
}

std::optional<XMLNode> LookupSourceIntervalIndex(const SourceIntervalIndex& index,
                                                 std::size_t offset) {
  const SourceNodeInterval* best = nullptr;
  for (const SourceNodeInterval& entry : index) {
    if (!ContainsOffset(entry.range, offset)) {
      continue;
    }

    if (best == nullptr || entry.depth > best->depth) {
      best = &entry;
    }
  }

  return best != nullptr ? std::make_optional(best->node) : std::nullopt;
}

std::optional<AttributeValueEdit> GetAttributeValueEdit(const XMLDocument& document,
                                                        SourceEditRange range) {
  if (document.source().empty() || range.start >= document.source().size()) {
    return std::nullopt;
  }

  const std::size_t lookupOffset = range.start;
  std::optional<XMLAttributeAtSourceOffset> attribute =
      document.attributeAtSourceOffset(lookupOffset);
  if (!attribute.has_value() || !attribute->location.start.offset.has_value() ||
      !attribute->location.end.offset.has_value()) {
    return std::nullopt;
  }

  const std::size_t attributeStart = *attribute->location.start.offset;
  const std::size_t attributeEnd = *attribute->location.end.offset;
  if (attributeEnd <= attributeStart || range.end > attributeEnd) {
    return std::nullopt;
  }

  if (!attribute->valueLocation.start.offset.has_value() ||
      !attribute->valueLocation.end.offset.has_value()) {
    return std::nullopt;
  }

  const std::size_t valueStart = *attribute->valueLocation.start.offset;
  const std::size_t valueEnd = *attribute->valueLocation.end.offset;
  if (range.start < valueStart || range.end > valueEnd) {
    return std::nullopt;
  }

  return AttributeValueEdit{
      .node = attribute->node,
      .name = attribute->name,
      .attributeLocation = attribute->location,
      .valueStart = valueStart,
      .valueEnd = valueEnd,
      .quote = attribute->quote,
  };
}

std::optional<OpeningTagEdit> GetOpeningTagEdit(const XMLDocument& document,
                                                SourceEditRange range) {
  if (document.source().empty() || range.start >= document.source().size()) {
    return std::nullopt;
  }

  std::optional<XMLNode> node = document.nodeAtSourceOffset(range.start);
  if (!node.has_value() || node->type() != XMLNode::Type::Element) {
    return std::nullopt;
  }

  std::optional<SourceRange> nodeLocation = node->getNodeLocation();
  if (!nodeLocation.has_value() || !nodeLocation->start.offset.has_value()) {
    return std::nullopt;
  }

  const std::size_t tagStart = *nodeLocation->start.offset;
  std::optional<std::size_t> tagEnd = FindOpeningTagEnd(document.source(), tagStart);
  if (!tagEnd.has_value() && nodeLocation->end.offset.has_value()) {
    tagEnd = FindDirtyOpeningTagEnd(document.source(), tagStart, *nodeLocation->end.offset);
  }
  if (range.start == tagStart && range.end == tagStart) {
    return std::nullopt;
  }
  if (tagEnd.has_value() && nodeLocation->end.offset.has_value() && range.start == tagStart &&
      range.end == *nodeLocation->end.offset && *tagEnd == *nodeLocation->end.offset) {
    return std::nullopt;
  }
  if (!tagEnd.has_value() || range.start < tagStart || range.end > *tagEnd) {
    return std::nullopt;
  }

  return OpeningTagEdit{
      .node = *node,
      .tagStart = tagStart,
      .tagEnd = *tagEnd,
  };
}

std::optional<TextNodeEdit> GetTextNodeEdit(const XMLDocument& document, SourceEditRange range) {
  if (document.source().empty() || range.start >= document.source().size()) {
    return std::nullopt;
  }

  std::optional<XMLNode> node = document.nodeAtSourceOffset(range.start);
  if (!node.has_value()) {
    return std::nullopt;
  }

  if (node->type() == XMLNode::Type::Data || IsRawTextLikeNode(node->type())) {
    std::optional<SourceEditRange> valueRange = ResolveNodeValueRange(*node, document.source());
    if (!valueRange.has_value() && node->type() == XMLNode::Type::Data) {
      std::optional<SourceRange> nodeLocation = node->getNodeLocation();
      valueRange = nodeLocation.has_value() ? ResolveEditRange(*nodeLocation, document.source())
                                            : std::nullopt;
    }

    if (!valueRange.has_value() || !ContainsEditRange(*valueRange, range)) {
      return std::nullopt;
    }

    return TextNodeEdit{
        .node = *node,
        .kind = IsRawTextLikeNode(node->type()) ? TextNodeEditKind::RawTextLikeNode
                                                : TextNodeEditKind::ParsedPcdata,
        .elementTextContent = false,
    };
  }

  if (node->type() != XMLNode::Type::Element || !node->value().has_value()) {
    return std::nullopt;
  }

  for (std::optional<XMLNode> child = node->firstChild(); child.has_value();
       child = child->nextSibling()) {
    if (child->type() == XMLNode::Type::Data || IsRawTextLikeNode(child->type())) {
      std::optional<SourceEditRange> childValueRange =
          ResolveNodeValueRange(*child, document.source());
      if (childValueRange.has_value() && ContainsEditRange(*childValueRange, range)) {
        return TextNodeEdit{
            .node = *child,
            .kind = IsRawTextLikeNode(child->type()) ? TextNodeEditKind::RawTextLikeNode
                                                     : TextNodeEditKind::ParsedPcdata,
            .elementTextContent = false,
        };
      }
    }

    if (child->type() == XMLNode::Type::Element) {
      return std::nullopt;
    }
  }

  std::optional<SourceEditRange> valueRange = ResolveNodeValueRange(*node, document.source());
  if (!valueRange.has_value()) {
    std::optional<SourceRange> nodeLocation = node->getNodeLocation();
    if (!nodeLocation.has_value() || !nodeLocation->start.offset.has_value() ||
        !nodeLocation->end.offset.has_value()) {
      return std::nullopt;
    }

    std::optional<std::size_t> tagEnd =
        FindOpeningTagEnd(document.source(), *nodeLocation->start.offset);
    std::optional<std::size_t> closingTagStart =
        FindClosingTagStart(document.source(), *nodeLocation->end.offset);
    if (!tagEnd.has_value() || !closingTagStart.has_value() || *closingTagStart < *tagEnd) {
      return std::nullopt;
    }

    valueRange = SourceEditRange{
        .start = *tagEnd,
        .end = *closingTagStart,
    };
  }

  if (!ContainsEditRange(*valueRange, range)) {
    return std::nullopt;
  }

  return TextNodeEdit{
      .node = *node,
      .kind = TextNodeEditKind::ParsedPcdata,
      .elementTextContent = true,
  };
}

std::optional<ElementSubtreeEdit> GetElementSubtreeEdit(const XMLDocument& document,
                                                        SourceEditRange range) {
  if (document.source().empty() || range.start > document.source().size()) {
    return std::nullopt;
  }

  std::size_t lookupOffset = range.start;
  if (lookupOffset == document.source().size()) {
    if (lookupOffset == 0) {
      return std::nullopt;
    }
    --lookupOffset;
  }

  std::optional<XMLNode> node = document.nodeAtSourceOffset(lookupOffset);
  while (node.has_value()) {
    if (node->type() == XMLNode::Type::Element) {
      std::optional<SourceRange> nodeLocation = node->getNodeLocation();
      if (nodeLocation.has_value() && nodeLocation->start.offset.has_value() &&
          nodeLocation->end.offset.has_value()) {
        std::optional<std::size_t> tagEnd =
            FindOpeningTagEnd(document.source(), *nodeLocation->start.offset);
        std::optional<std::size_t> closingTagStart =
            FindClosingTagStart(document.source(), *nodeLocation->end.offset);
        if (tagEnd.has_value() && closingTagStart.has_value() && range.start >= *tagEnd &&
            range.end <= *closingTagStart) {
          return ElementSubtreeEdit{
              .node = *node,
          };
        }
      }
    }

    node = node->parentElement();
  }

  return std::nullopt;
}

std::optional<SourceEditRange> GetTextNodeSourceRange(const XMLDocument& document,
                                                      const TextNodeEdit& edit) {
  if (std::optional<SourceEditRange> valueRange =
          ResolveNodeValueRange(edit.node, document.source())) {
    return valueRange;
  }

  std::optional<SourceRange> nodeLocation = edit.node.getNodeLocation();
  if (!nodeLocation.has_value() || !nodeLocation->start.offset.has_value() ||
      !nodeLocation->end.offset.has_value()) {
    return std::nullopt;
  }

  if (!edit.elementTextContent) {
    return SourceEditRange{
        .start = *nodeLocation->start.offset,
        .end = *nodeLocation->end.offset,
    };
  }

  std::optional<std::size_t> tagEnd =
      FindOpeningTagEnd(document.source(), *nodeLocation->start.offset);
  std::optional<std::size_t> closingTagStart =
      FindClosingTagStart(document.source(), *nodeLocation->end.offset);
  if (!tagEnd.has_value() || !closingTagStart.has_value() || *closingTagStart < *tagEnd) {
    return std::nullopt;
  }

  return SourceEditRange{
      .start = *tagEnd,
      .end = *closingTagStart,
  };
}

AttributeMap BuildAttributeMap(const XMLNode& node) {
  AttributeMap result;
  for (const XMLQualifiedNameRef& name : node.attributes()) {
    std::optional<RcString> value = node.getAttribute(name);
    if (value.has_value()) {
      result.emplace(XMLQualifiedName(RcString(name.namespacePrefix), RcString(name.name)), *value);
    }
  }

  return result;
}

ParseDiagnostic MakeEditDiagnostic(RcString reason, SourceRange range) {
  return ParseDiagnostic::Error(std::move(reason), range);
}

bool DiagnosticEquals(const std::optional<ParseDiagnostic>& lhs,
                      const std::optional<ParseDiagnostic>& rhs) {
  if (lhs.has_value() != rhs.has_value()) {
    return false;
  }

  if (!lhs.has_value()) {
    return true;
  }

  return lhs->severity == rhs->severity && lhs->reason == rhs->reason && lhs->range == rhs->range;
}

void UpdateSourceDiagnostic(XMLDocument& document, ApplySourceEditResult& result,
                            const XMLNode& node, std::optional<ParseDiagnostic> diagnostic) {
  auto& context = document.registry().ctx().get<XMLDocumentContext>();
  if (DiagnosticEquals(context.sourceDiagnostic, diagnostic)) {
    return;
  }

  context.sourceDiagnostic = diagnostic;
  result.mutations.push_back(XMLMutation{
      .kind = XMLMutation::Kind::SourceDiagnosticChanged,
      .node = node,
      .attributeName = XMLQualifiedName(""),
      .value = std::nullopt,
      .diagnostic = std::move(diagnostic),
      .scope = result.scope,
  });
}

SourceRange MakeNodeDiagnosticRange(const XMLNode& node) {
  return node.getNodeLocation().value_or(
      SourceRange{FileOffset::EndOfString(), FileOffset::EndOfString()});
}

void ClearSourceLocationsRecursive(XMLNode node) {
  for (std::optional<XMLNode> child = node.firstChild(); child.has_value();) {
    XMLNode currentChild = *child;
    child = currentChild.nextSibling();
    ClearSourceLocationsRecursive(currentChild);
  }

  node.clearSourceLocation();
}

XMLQualifiedName MakeOwnedName(const XMLQualifiedNameRef& name) {
  return XMLQualifiedName(RcString(name.namespacePrefix), RcString(name.name));
}

bool IsDocumentNode(const XMLDocument& document, const XMLNode& node) {
  return node.entityHandle().registry() == document.sharedRegistry().get();
}

bool IsAncestorOf(const XMLNode& ancestor, const XMLNode& node) {
  for (std::optional<XMLNode> current = node.parentElement(); current.has_value();
       current = current->parentElement()) {
    if (*current == ancestor) {
      return true;
    }
  }

  return false;
}

std::optional<AttributeValueEdit> GetAttributeValueEditForNode(const XMLDocument& document,
                                                               const XMLNode& node,
                                                               const XMLQualifiedName& name) {
  if (std::optional<XMLAttributeSourceLocation> sourceLocation =
          node.getAttributeSourceLocation(name)) {
    if (!sourceLocation->valueRange.start.offset.has_value() ||
        !sourceLocation->valueRange.end.offset.has_value()) {
      return std::nullopt;
    }

    return AttributeValueEdit{
        .node = node,
        .name = name,
        .attributeLocation = sourceLocation->fullRange,
        .valueStart = *sourceLocation->valueRange.start.offset,
        .valueEnd = *sourceLocation->valueRange.end.offset,
        .quote = sourceLocation->quote,
    };
  }

  std::optional<SourceRange> attributeLocation = node.getAttributeLocation(document.source(), name);
  if (!attributeLocation.has_value()) {
    return std::nullopt;
  }

  return GetAttributeValueSpan(node, name, *attributeLocation, document.source());
}

void RefreshAttributeSourceLocation(const XMLDocument& document, XMLNode& node,
                                    const XMLQualifiedName& name) {
  std::optional<SourceRange> attributeLocation = node.getAttributeLocation(document.source(), name);
  if (!attributeLocation.has_value()) {
    node.clearAttributeSourceLocation(name);
    return;
  }

  std::optional<AttributeValueEdit> valueEdit =
      GetAttributeValueSpan(node, name, *attributeLocation, document.source());
  if (!valueEdit.has_value()) {
    node.clearAttributeSourceLocation(name);
    return;
  }

  node.setAttributeSourceLocation(name, *attributeLocation,
                                  SourceRange{FileOffset::Offset(valueEdit->valueStart),
                                              FileOffset::Offset(valueEdit->valueEnd)},
                                  valueEdit->quote);
}

std::optional<std::size_t> GetAttributeInsertionOffset(const XMLDocument& document,
                                                       const XMLNode& node) {
  std::optional<SourceRange> nodeLocation = node.getNodeLocation();
  if (!nodeLocation.has_value() || !nodeLocation->start.offset.has_value()) {
    return std::nullopt;
  }

  std::optional<std::size_t> tagEnd =
      FindOpeningTagEnd(document.source(), *nodeLocation->start.offset);
  if (!tagEnd.has_value() || *tagEnd == 0) {
    return std::nullopt;
  }

  if (*tagEnd >= 2 &&
      (document.source()[*tagEnd - 2] == '/' || document.source()[*tagEnd - 2] == '?')) {
    return *tagEnd - 2;
  }

  return *tagEnd - 1;
}

std::string SerializeAttributeInsertion(std::string_view source, std::size_t insertionOffset,
                                        const XMLQualifiedName& name,
                                        std::string_view escapedValue) {
  std::string result;
  if (insertionOffset == 0 || !IsXmlWhitespace(source[insertionOffset - 1])) {
    result.push_back(' ');
  }
  AppendQualifiedName(result, name);
  result.append("=\"");
  result.append(escapedValue);
  result.push_back('"');
  return result;
}

std::optional<SourceEditRange> GetAttributeRemovalRange(const XMLDocument& document,
                                                        const XMLNode& node,
                                                        const XMLQualifiedName& name) {
  std::optional<SourceRange> attributeLocation = node.getAttributeLocation(document.source(), name);
  std::optional<SourceEditRange> attributeRange =
      attributeLocation.has_value() ? ResolveEditRange(*attributeLocation, document.source())
                                    : std::nullopt;
  if (!attributeRange.has_value()) {
    return std::nullopt;
  }

  SourceEditRange removalRange = *attributeRange;
  while (removalRange.start > 0 && IsXmlWhitespace(document.source()[removalRange.start - 1])) {
    --removalRange.start;
  }

  return removalRange;
}

struct NodeInsertionPlan {
  std::size_t replacementOffset = 0;
  std::size_t replacementLength = 0;
  std::string prefix;
  std::string suffix;
  std::size_t insertedNodeOffset = 0;
  std::optional<SourceRange> parentOpeningTagLocation;
  std::optional<SourceRange> parentClosingTagLocation;
  std::optional<FileOffset> parentEndOffset;
};

std::optional<SourceRange> ShiftRangeLeft(SourceRange range, std::size_t amount) {
  if (!range.start.offset.has_value() || !range.end.offset.has_value() ||
      *range.start.offset < amount || *range.end.offset < amount) {
    return std::nullopt;
  }

  return SourceRange{
      FileOffset::Offset(*range.start.offset - amount),
      FileOffset::Offset(*range.end.offset - amount),
  };
}

bool ShiftPlanLeft(NodeInsertionPlan& plan, std::size_t amount) {
  if (plan.replacementOffset < amount || plan.insertedNodeOffset < amount) {
    return false;
  }

  plan.replacementOffset -= amount;
  plan.insertedNodeOffset -= amount;

  if (plan.parentOpeningTagLocation.has_value()) {
    std::optional<SourceRange> shifted = ShiftRangeLeft(*plan.parentOpeningTagLocation, amount);
    if (!shifted.has_value()) {
      return false;
    }
    plan.parentOpeningTagLocation = *shifted;
  }

  if (plan.parentClosingTagLocation.has_value()) {
    std::optional<SourceRange> shifted = ShiftRangeLeft(*plan.parentClosingTagLocation, amount);
    if (!shifted.has_value()) {
      return false;
    }
    plan.parentClosingTagLocation = *shifted;
  }

  if (plan.parentEndOffset.has_value()) {
    if (!plan.parentEndOffset->offset.has_value() || *plan.parentEndOffset->offset < amount) {
      return false;
    }
    plan.parentEndOffset = FileOffset::Offset(*plan.parentEndOffset->offset - amount);
  }

  return true;
}

bool IsXmlSpace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

std::string ClosingTagFor(const XMLNode& node) {
  std::string closingTag;
  closingTag.append("</");
  AppendQualifiedName(closingTag, node.tagName());
  closingTag.push_back('>');
  return closingTag;
}

bool SourceHasClosingTagAt(std::string_view source, std::size_t offset,
                           std::string_view closingTag) {
  return offset <= source.size() && closingTag.size() <= source.size() - offset &&
         source.substr(offset, closingTag.size()) == closingTag;
}

// How a structural mutation should format the whitespace around a newly serialized node.
enum class InsertionFormatting {
  // Text content: keep it inline (e.g. `<text>hello</text>`). Never introduces newlines,
  // matching how a human authors a text run.
  kInline,
  // Structural child (element inserted/moved via the layer tree): lay the child out on its
  // own indented line when the surrounding source is already spread across multiple lines,
  // and stay compact when the author wrote it on a single line.
  kStructural,
};

// The indentation (a run of spaces/tabs) of the line that `offset` begins on: the
// whitespace between the newline preceding `offset` and `offset` itself. Returns nullopt
// when `offset` is not at the start of its line (an inline / single-line context), which
// signals that structural insertion should stay compact and synthesize no whitespace.
std::optional<std::string_view> LineIndentBefore(std::string_view source, std::size_t offset) {
  if (offset > source.size()) {
    return std::nullopt;
  }
  std::size_t indentStart = offset;
  while (indentStart > 0 && (source[indentStart - 1] == ' ' || source[indentStart - 1] == '\t')) {
    --indentStart;
  }
  if (indentStart == 0 || source[indentStart - 1] != '\n') {
    return std::nullopt;
  }
  return source.substr(indentStart, offset - indentStart);
}

// The leading indentation (run of spaces/tabs at the start of the line) of the line that
// contains `pos`. Unlike LineIndentBefore, `pos` may sit anywhere on the line.
std::string_view LineIndentContaining(std::string_view source, std::size_t pos) {
  if (pos > source.size()) {
    pos = source.size();
  }
  std::size_t lineStart = pos;
  while (lineStart > 0 && source[lineStart - 1] != '\n') {
    --lineStart;
  }
  std::size_t indentEnd = lineStart;
  while (indentEnd < source.size() && (source[indentEnd] == ' ' || source[indentEnd] == '\t')) {
    ++indentEnd;
  }
  return source.substr(lineStart, indentEnd - lineStart);
}

// Detect the document's indentation unit (a tab, or a run of N spaces) from the first
// indented, non-blank line. Falls back to two spaces when there is no indentation to learn
// from. This keeps synthesized indentation faithful to the author's chosen style (tabs vs
// spaces, and width) instead of imposing a global default.
std::string DetectIndentUnit(std::string_view source) {
  const std::size_t size = source.size();
  std::size_t i = 0;
  while (i < size) {
    if (source[i] != '\n') {
      ++i;
      continue;
    }
    ++i;  // Move past the newline to the start of the next line.
    std::size_t j = i;
    while (j < size && (source[j] == ' ' || source[j] == '\t')) {
      ++j;
    }
    if (j > i && j < size && source[j] != '\n' && source[j] != '\r') {
      return std::string(source.substr(i, j - i));
    }
    i = j;
  }
  return "  ";
}

std::optional<std::size_t> FindParentClosingTagInsertionOffset(const XMLDocument& document,
                                                               const XMLNode& parent) {
  const std::string closingTag = ClosingTagFor(parent);
  if (std::optional<SourceRange> closingTagLocation = parent.getClosingTagLocation();
      closingTagLocation.has_value() && closingTagLocation->start.offset.has_value()) {
    const std::size_t offset = *closingTagLocation->start.offset;
    if (SourceHasClosingTagAt(document.source(), offset, closingTag)) {
      return offset;
    }
  }

  std::optional<SourceRange> nodeLocation = parent.getNodeLocation();
  if (!nodeLocation.has_value() || !nodeLocation->start.offset.has_value() ||
      !nodeLocation->end.offset.has_value() ||
      *nodeLocation->end.offset > document.source().size()) {
    return std::nullopt;
  }

  const std::size_t searchStart = *nodeLocation->start.offset;
  const std::size_t searchEnd = *nodeLocation->end.offset;
  if (searchEnd < closingTag.size()) {
    return std::nullopt;
  }

  const std::size_t lastPossibleOffset = searchEnd - closingTag.size();
  for (std::size_t offset = lastPossibleOffset;; --offset) {
    if (SourceHasClosingTagAt(document.source(), offset, closingTag)) {
      return offset;
    }
    if (offset == searchStart) {
      break;
    }
  }

  return std::nullopt;
}

void ApplyParentInsertionPlan(XMLNode& parent, const NodeInsertionPlan& plan) {
  if (plan.parentOpeningTagLocation.has_value() && plan.parentClosingTagLocation.has_value() &&
      plan.parentEndOffset.has_value()) {
    parent.setOpeningTagLocation(*plan.parentOpeningTagLocation);
    parent.setClosingTagLocation(*plan.parentClosingTagLocation);
    parent.setSourceEndOffset(*plan.parentEndOffset);
  }
}

std::optional<NodeInsertionPlan> GetNodeInsertionPlan(const XMLDocument& document,
                                                      const XMLNode& parent,
                                                      const std::optional<XMLNode>& referenceNode,
                                                      std::string_view serializedNode,
                                                      InsertionFormatting formatting) {
  const std::string_view source = document.source();
  const bool structural = (formatting == InsertionFormatting::kStructural);

  // Scenario A: insert before an existing sibling. The whitespace already in front of the
  // reference node becomes the new node's leading indentation; a trailing newline plus the
  // reference node's own indentation pushes the reference node back onto its own line.
  if (referenceNode.has_value()) {
    std::optional<XMLNode> referenceParent = referenceNode->parentElement();
    if (!referenceParent.has_value() || *referenceParent != parent) {
      return std::nullopt;
    }

    std::optional<SourceRange> referenceLocation = referenceNode->getNodeLocation();
    if (!referenceLocation.has_value() || !referenceLocation->start.offset.has_value()) {
      return std::nullopt;
    }

    const std::size_t offset = *referenceLocation->start.offset;
    std::string suffix;
    if (structural) {
      if (std::optional<std::string_view> refIndent = LineIndentBefore(source, offset)) {
        suffix.push_back('\n');
        suffix.append(*refIndent);
      }
    }
    return NodeInsertionPlan{
        .replacementOffset = offset,
        .replacementLength = 0,
        .suffix = std::move(suffix),
        .insertedNodeOffset = offset,
    };
  }

  // Scenario B: append as the last child, immediately before the parent's closing tag.
  if (std::optional<std::size_t> closingTagOffset =
          FindParentClosingTagInsertionOffset(document, parent);
      closingTagOffset.has_value()) {
    const std::size_t offset = *closingTagOffset;

    if (structural) {
      // Append the child on its own line right after the last existing sibling, leaving the
      // whitespace that already indents the closing tag untouched. This stays a pure
      // insertion (nothing removed), which the text-mirror's echo-suppression relies on.
      std::size_t wsStart = offset;
      while (wsStart > 0 && IsXmlSpace(source[wsStart - 1])) {
        --wsStart;
      }
      const std::string_view whitespace = source.substr(wsStart, offset - wsStart);
      if (whitespace.find('\n') != std::string_view::npos) {
        const std::string_view closingIndent = LineIndentContaining(source, offset);
        std::string_view childIndent = LineIndentContaining(source, wsStart);
        std::string childIndentStorage;
        if (childIndent.size() <= closingIndent.size()) {
          // No deeper existing sibling to match (an empty but multi-line parent): indent one
          // detected unit past the closing tag.
          childIndentStorage = std::string(closingIndent) + DetectIndentUnit(source);
          childIndent = childIndentStorage;
        }

        std::string prefix = "\n";
        prefix.append(childIndent);

        return NodeInsertionPlan{
            .replacementOffset = wsStart,
            .replacementLength = 0,
            .prefix = std::move(prefix),
            .insertedNodeOffset = wsStart + 1 + childIndent.size(),
        };
      }
    }

    return NodeInsertionPlan{
        .replacementOffset = offset,
        .replacementLength = 0,
        .insertedNodeOffset = offset,
    };
  }

  // Scenario C: the parent is self-closing (`<g/>`); expand it into an open/close pair with
  // the new child inside.
  std::optional<SourceRange> nodeLocation = parent.getNodeLocation();
  if (!nodeLocation.has_value() || !nodeLocation->start.offset.has_value() ||
      !nodeLocation->end.offset.has_value() || *nodeLocation->end.offset < 2 ||
      *nodeLocation->end.offset > source.size()) {
    return std::nullopt;
  }

  const std::size_t selfCloseStart = *nodeLocation->end.offset - 2;
  if (source.substr(selfCloseStart, 2) != "/>") {
    return std::nullopt;
  }

  std::size_t replacementStart = selfCloseStart;
  while (replacementStart > *nodeLocation->start.offset &&
         IsXmlSpace(source[replacementStart - 1])) {
    --replacementStart;
  }

  const std::string closingTag = ClosingTagFor(parent);

  std::string prefix = ">";
  std::string suffix = closingTag;
  if (structural) {
    if (std::optional<std::string_view> parentIndent =
            LineIndentBefore(source, *nodeLocation->start.offset)) {
      const std::string childIndent = std::string(*parentIndent) + DetectIndentUnit(source);
      prefix = ">\n" + childIndent;
      suffix = "\n" + std::string(*parentIndent) + closingTag;
    }
  }

  const std::size_t openingTagEnd = replacementStart + 1;  // Just past the emitted '>'.
  const std::size_t insertedOffset = replacementStart + prefix.size();
  const std::size_t closingTagStart =
      insertedOffset + serializedNode.size() + (suffix.size() - closingTag.size());
  return NodeInsertionPlan{
      .replacementOffset = replacementStart,
      .replacementLength = *nodeLocation->end.offset - replacementStart,
      .prefix = std::move(prefix),
      .suffix = std::move(suffix),
      .insertedNodeOffset = insertedOffset,
      .parentOpeningTagLocation = SourceRange{FileOffset::Offset(*nodeLocation->start.offset),
                                              FileOffset::Offset(openingTagEnd)},
      .parentClosingTagLocation =
          SourceRange{FileOffset::Offset(closingTagStart),
                      FileOffset::Offset(closingTagStart + closingTag.size())},
      .parentEndOffset = FileOffset::Offset(closingTagStart + closingTag.size()),
  };
}

SourceRange OffsetRange(SourceRange range, std::size_t sourceOffsetBase);

void SetSourceOffsetsFromParsed(XMLNode& node, const XMLNode& parsedNode,
                                std::size_t sourceOffsetBase) {
  std::optional<SourceRange> location = parsedNode.getNodeLocation();
  if (!location.has_value() || !location->start.offset.has_value() ||
      !location->end.offset.has_value()) {
    return;
  }

  node.setSourceStartOffset(FileOffset::Offset(sourceOffsetBase + *location->start.offset));
  node.setSourceEndOffset(FileOffset::Offset(sourceOffsetBase + *location->end.offset));

  if (std::optional<SourceRange> openingRange = parsedNode.getOpeningTagLocation()) {
    node.setOpeningTagLocation(OffsetRange(*openingRange, sourceOffsetBase));
  } else {
    node.clearOpeningTagLocation();
  }

  if (std::optional<SourceRange> closingRange = parsedNode.getClosingTagLocation()) {
    node.setClosingTagLocation(OffsetRange(*closingRange, sourceOffsetBase));
  } else {
    node.clearClosingTagLocation();
  }

  if (std::optional<SourceRange> valueRange = parsedNode.getValueLocation()) {
    node.setValueLocation(OffsetRange(*valueRange, sourceOffsetBase));
  } else {
    node.clearValueLocation();
  }
}

SourceRange OffsetRange(SourceRange range, std::size_t sourceOffsetBase) {
  if (!range.start.offset.has_value() || !range.end.offset.has_value()) {
    return range;
  }

  return SourceRange{
      FileOffset::Offset(sourceOffsetBase + *range.start.offset),
      FileOffset::Offset(sourceOffsetBase + *range.end.offset),
  };
}

void SyncAttributeSourceLocationsFromParsed(XMLNode& target, const XMLNode& parsedNode,
                                            std::size_t sourceOffsetBase) {
  for (const XMLQualifiedNameRef& name : target.attributes()) {
    if (!parsedNode.hasAttribute(name)) {
      target.clearAttributeSourceLocation(name);
    }
  }

  for (const XMLQualifiedNameRef& name : parsedNode.attributes()) {
    std::optional<XMLAttributeSourceLocation> parsedLocation =
        parsedNode.getAttributeSourceLocation(name);
    if (!parsedLocation.has_value()) {
      target.clearAttributeSourceLocation(name);
      continue;
    }

    target.setAttributeSourceLocation(
        name, OffsetRange(parsedLocation->fullRange, sourceOffsetBase),
        OffsetRange(parsedLocation->valueRange, sourceOffsetBase), parsedLocation->quote);
  }
}

void SyncAttributesFromParsed(XMLNode& target, const XMLNode& parsedNode,
                              std::vector<XMLMutation>* mutations = nullptr,
                              ReparseScope mutationScope = ReparseScope::Document) {
  const AttributeMap currentAttributes = BuildAttributeMap(target);
  const AttributeMap parsedAttributes = BuildAttributeMap(parsedNode);

  for (const auto& [name, value] : currentAttributes) {
    if (!parsedAttributes.contains(name)) {
      target.removeAttribute(name);
      if (mutations != nullptr) {
        mutations->push_back(XMLMutation{
            .kind = XMLMutation::Kind::AttributeRemoved,
            .node = target,
            .attributeName = name,
            .value = std::nullopt,
            .scope = mutationScope,
        });
      }
    }
  }

  for (const auto& [name, value] : parsedAttributes) {
    const auto currentIt = currentAttributes.find(name);
    if (currentIt == currentAttributes.end() || currentIt->second != value) {
      target.setAttribute(name, value);
      if (mutations != nullptr) {
        mutations->push_back(XMLMutation{
            .kind = XMLMutation::Kind::AttributeSet,
            .node = target,
            .attributeName = name,
            .value = value,
            .scope = mutationScope,
        });
      }
    }
  }
}

void SyncValueFromParsed(XMLNode& target, const XMLNode& parsedNode,
                         std::vector<XMLMutation>* mutations = nullptr,
                         ReparseScope mutationScope = ReparseScope::Document) {
  const std::optional<RcString> previousValue = target.value();
  std::optional<RcString> parsedValue = parsedNode.value();
  if (parsedValue.has_value()) {
    target.setValue(*parsedValue);
  } else if (target.entityHandle().all_of<components::XMLValueComponent>()) {
    target.entityHandle().remove<components::XMLValueComponent>();
  }

  const std::optional<RcString> updatedValue = target.value();
  if (mutations != nullptr && previousValue != updatedValue) {
    mutations->push_back(XMLMutation{
        .kind = XMLMutation::Kind::NodeValueChanged,
        .node = target,
        .attributeName = XMLQualifiedName(""),
        .value = updatedValue.value_or(RcString("")),
        .scope = mutationScope,
    });
  }
}

std::optional<RcString> ElementId(const XMLNode& node) {
  if (node.type() != XMLNode::Type::Element) {
    return std::nullopt;
  }

  return node.getAttribute("id");
}

std::optional<std::size_t> FindReusableChild(const XMLNode& parsedChild,
                                             const std::vector<XMLNode>& oldChildren,
                                             const std::vector<bool>& usedChildren) {
  std::optional<RcString> parsedId = ElementId(parsedChild);
  if (!parsedId.has_value() || parsedId->empty()) {
    return std::nullopt;
  }

  for (std::size_t index = 0; index < oldChildren.size(); ++index) {
    if (usedChildren[index]) {
      continue;
    }

    const XMLNode& oldChild = oldChildren[index];
    std::optional<RcString> oldId = ElementId(oldChild);
    if (oldChild.type() == parsedChild.type() && oldChild.tagName() == parsedChild.tagName() &&
        oldId.has_value() && *oldId == *parsedId) {
      return index;
    }
  }

  return std::nullopt;
}

bool CheckedAccumulate(std::uint64_t& total, std::uint64_t additional) {
  if (additional > std::numeric_limits<std::uint64_t>::max() - total) {
    return false;
  }
  total += additional;
  return true;
}

std::optional<std::uint64_t> CountSubtreeNodes(const XMLNode& root) {
  std::uint64_t count = 0;
  std::vector<XMLNode> stack{root};
  while (!stack.empty()) {
    XMLNode node = std::move(stack.back());
    stack.pop_back();
    if (!CheckedAccumulate(count, 1)) {
      return std::nullopt;
    }
    for (std::optional<XMLNode> child = node.firstChild(); child.has_value();
         child = child->nextSibling()) {
      stack.push_back(*child);
    }
  }
  return count;
}

std::optional<std::uint64_t> CountNewNodesRequired(const XMLNode& target,
                                                   const XMLNode& parsedTarget) {
  std::vector<XMLNode> oldChildren;
  for (std::optional<XMLNode> child = target.firstChild(); child.has_value();
       child = child->nextSibling()) {
    oldChildren.push_back(*child);
  }
  std::vector<bool> usedChildren(oldChildren.size(), false);

  std::uint64_t required = 0;
  for (std::optional<XMLNode> parsedChild = parsedTarget.firstChild(); parsedChild.has_value();
       parsedChild = parsedChild->nextSibling()) {
    if (std::optional<std::size_t> oldIndex =
            FindReusableChild(*parsedChild, oldChildren, usedChildren)) {
      usedChildren[*oldIndex] = true;
      std::optional<std::uint64_t> nested =
          CountNewNodesRequired(oldChildren[*oldIndex], *parsedChild);
      if (!nested.has_value() || !CheckedAccumulate(required, *nested)) {
        return std::nullopt;
      }
    } else {
      std::optional<std::uint64_t> subtreeNodes = CountSubtreeNodes(*parsedChild);
      if (!subtreeNodes.has_value() || !CheckedAccumulate(required, *subtreeNodes)) {
        return std::nullopt;
      }
    }
  }
  return required;
}

std::uint64_t CountLiveXmlNodes(XMLDocument& document) {
  std::uint64_t count = 0;
  Registry& registry = document.registry();
  for (Entity entity : registry.view<donner::components::TreeComponent>()) {
    std::optional<XMLNode> node = XMLNode::TryCast(EntityHandle(registry, entity));
    if (node.has_value() && node->type() != XMLNode::Type::Document) {
      if (count == std::numeric_limits<std::uint64_t>::max()) {
        return count;
      }
      ++count;
    }
  }
  return count;
}

std::size_t ElementDepth(const XMLNode& node) {
  std::size_t depth = node.type() == XMLNode::Type::Element ? 1 : 0;
  for (std::optional<XMLNode> parent = node.parentElement(); parent.has_value();
       parent = parent->parentElement()) {
    if (parent->type() == XMLNode::Type::Element) {
      if (depth == std::numeric_limits<std::size_t>::max()) {
        return depth;
      }
      ++depth;
    }
  }
  return depth;
}

std::size_t MaximumElementDepth(const XMLNode& root, std::size_t rootDepth,
                                Entity skippedSubtree = entt::null) {
  struct PendingNode {
    XMLNode node;
    std::size_t parentDepth = 0;
  };

  std::size_t maximumDepth = 0;
  const std::size_t rootParentDepth =
      root.type() == XMLNode::Type::Element && rootDepth > 0 ? rootDepth - 1 : rootDepth;
  std::vector<PendingNode> stack{{root, rootParentDepth}};
  while (!stack.empty()) {
    PendingNode pending = std::move(stack.back());
    stack.pop_back();
    if (pending.node.entityHandle().entity() == skippedSubtree) {
      continue;
    }

    std::size_t depth = pending.parentDepth;
    if (pending.node.type() == XMLNode::Type::Element) {
      if (depth == std::numeric_limits<std::size_t>::max()) {
        return depth;
      }
      ++depth;
      maximumDepth = std::max(maximumDepth, depth);
    }
    for (std::optional<XMLNode> child = pending.node.firstChild(); child.has_value();
         child = child->nextSibling()) {
      stack.push_back(PendingNode{*child, depth});
    }
  }
  return maximumDepth;
}

std::uint64_t AttributeCount(const XMLNode& node) {
  const auto* attributes = node.entityHandle().try_get<donner::components::AttributesComponent>();
  return attributes != nullptr ? static_cast<std::uint64_t>(attributes->attributeCount()) : 0;
}

std::optional<std::uint64_t> CountSubtreeAttributes(const XMLNode& root) {
  std::uint64_t count = 0;
  std::vector<XMLNode> stack{root};
  while (!stack.empty()) {
    XMLNode node = std::move(stack.back());
    stack.pop_back();
    if (!CheckedAccumulate(count, AttributeCount(node))) {
      return std::nullopt;
    }
    for (std::optional<XMLNode> child = node.firstChild(); child.has_value();
         child = child->nextSibling()) {
      stack.push_back(*child);
    }
  }
  return count;
}

std::optional<std::uint64_t> CountLiveXmlAttributes(XMLDocument& document) {
  std::uint64_t count = 0;
  Registry& registry = document.registry();
  for (Entity entity : registry.view<donner::components::AttributesComponent>()) {
    std::optional<XMLNode> node = XMLNode::TryCast(EntityHandle(registry, entity));
    if (node.has_value() && node->type() != XMLNode::Type::Document &&
        !CheckedAccumulate(
            count,
            static_cast<std::uint64_t>(
                registry.get<donner::components::AttributesComponent>(entity).attributeCount()))) {
      return std::nullopt;
    }
  }
  return count;
}

bool ReplaceAttributeCount(std::uint64_t& total, std::uint64_t oldCount, std::uint64_t newCount) {
  if (oldCount > total) {
    return false;
  }
  total -= oldCount;
  return CheckedAccumulate(total, newCount);
}

bool ApplySyncedSubtreeAttributeCount(std::uint64_t& total, const XMLNode& target,
                                      const XMLNode& parsedTarget);

bool ApplyReplacedChildrenAttributeCount(std::uint64_t& total, const XMLNode& target,
                                         const XMLNode& parsedTarget) {
  std::vector<XMLNode> oldChildren;
  for (std::optional<XMLNode> child = target.firstChild(); child.has_value();
       child = child->nextSibling()) {
    oldChildren.push_back(*child);
  }
  std::vector<bool> usedChildren(oldChildren.size(), false);

  for (std::optional<XMLNode> parsedChild = parsedTarget.firstChild(); parsedChild.has_value();
       parsedChild = parsedChild->nextSibling()) {
    if (std::optional<std::size_t> oldIndex =
            FindReusableChild(*parsedChild, oldChildren, usedChildren)) {
      usedChildren[*oldIndex] = true;
      if (!ApplySyncedSubtreeAttributeCount(total, oldChildren[*oldIndex], *parsedChild)) {
        return false;
      }
    } else {
      std::optional<std::uint64_t> added = CountSubtreeAttributes(*parsedChild);
      if (!added.has_value() || !CheckedAccumulate(total, *added)) {
        return false;
      }
    }
  }
  return true;
}

bool ApplySyncedSubtreeAttributeCount(std::uint64_t& total, const XMLNode& target,
                                      const XMLNode& parsedTarget) {
  return ReplaceAttributeCount(total, AttributeCount(target), AttributeCount(parsedTarget)) &&
         ApplyReplacedChildrenAttributeCount(total, target, parsedTarget);
}

std::optional<ParseDiagnostic> ValidateOpeningTagAttributeLimit(XMLDocument& document,
                                                                const XMLNode& target,
                                                                const XMLNode& parsedTarget,
                                                                SourceRange diagnosticRange) {
  const auto& context = document.registry().ctx().get<XMLDocumentContext>();
  std::optional<std::uint64_t> prospectiveAttributes = CountLiveXmlAttributes(document);
  if (!prospectiveAttributes.has_value() ||
      !ReplaceAttributeCount(*prospectiveAttributes, AttributeCount(target),
                             AttributeCount(parsedTarget)) ||
      *prospectiveAttributes > context.maximumSourceEditTotalAttributes) {
    return MakeEditDiagnostic("Incremental source edit exceeds the document total-attribute limit",
                              diagnosticRange);
  }
  return std::nullopt;
}

std::optional<ParseDiagnostic> ValidateIncrementalTreeLimits(XMLDocument& document,
                                                             const XMLNode& target,
                                                             const XMLNode& parsedTarget,
                                                             SourceRange diagnosticRange) {
  const auto& context = document.registry().ctx().get<XMLDocumentContext>();
  const std::optional<std::uint64_t> requiredNewNodes = CountNewNodesRequired(target, parsedTarget);
  const std::uint64_t liveNodes = CountLiveXmlNodes(document);
  if (!requiredNewNodes.has_value() || liveNodes > context.maximumSourceEditTreeNodes ||
      *requiredNewNodes > context.maximumSourceEditTreeNodes - liveNodes) {
    return MakeEditDiagnostic("Incremental source edit exceeds the document tree-node limit",
                              diagnosticRange);
  }

  const std::size_t targetDepth = ElementDepth(target);
  const std::size_t relativeReplacementDepth = MaximumElementDepth(parsedTarget, 1);
  if (targetDepth == 0 || relativeReplacementDepth == 0 ||
      relativeReplacementDepth - 1 > std::numeric_limits<std::size_t>::max() - targetDepth) {
    return MakeEditDiagnostic("Incremental source edit exceeds the document tree-depth limit",
                              diagnosticRange);
  }
  const std::size_t replacementDepth = targetDepth + relativeReplacementDepth - 1;
  const std::size_t outsideDepth =
      MaximumElementDepth(document.root(), 0, target.entityHandle().entity());
  if (context.maximumSourceEditTreeDepth < 0 ||
      std::max(outsideDepth, replacementDepth) >
          static_cast<std::size_t>(context.maximumSourceEditTreeDepth)) {
    return MakeEditDiagnostic("Incremental source edit exceeds the document tree-depth limit",
                              diagnosticRange);
  }

  std::optional<std::uint64_t> prospectiveAttributes = CountLiveXmlAttributes(document);
  if (!prospectiveAttributes.has_value() ||
      !ApplyReplacedChildrenAttributeCount(*prospectiveAttributes, target, parsedTarget) ||
      *prospectiveAttributes > context.maximumSourceEditTotalAttributes) {
    return MakeEditDiagnostic("Incremental source edit exceeds the document total-attribute limit",
                              diagnosticRange);
  }

  return std::nullopt;
}

XMLParser::Options IncrementalPreflightOptions(XMLDocument& document,
                                               std::size_t maximumInputSize) {
  const auto& context = document.registry().ctx().get<XMLDocumentContext>();
  XMLParser::Options options;
  options.maximumInputSize = maximumInputSize;
  options.maxElements = context.maximumSourceEditTreeNodes;
  options.maxNestingDepth = context.maximumSourceEditTreeDepth;
  options.maxTotalAttributes = context.maximumSourceEditTotalAttributes;
  return options;
}

XMLNode CloneParsedNodeInto(XMLDocument& document, const XMLNode& parsedNode,
                            std::size_t sourceOffsetBase);

void ReplaceChildrenFromParsedNode(XMLDocument& document, XMLNode& target,
                                   const XMLNode& parsedTarget, std::size_t sourceOffsetBase,
                                   std::vector<XMLMutation>* mutations = nullptr,
                                   ReparseScope mutationScope = ReparseScope::Document);

void SyncNodeFromParsed(XMLDocument& document, XMLNode& target, const XMLNode& parsedNode,
                        std::size_t sourceOffsetBase, std::vector<XMLMutation>* mutations = nullptr,
                        ReparseScope mutationScope = ReparseScope::Document) {
  if (target.type() == XMLNode::Type::Element || target.type() == XMLNode::Type::XMLDeclaration) {
    SyncAttributesFromParsed(target, parsedNode, mutations, mutationScope);
    SyncAttributeSourceLocationsFromParsed(target, parsedNode, sourceOffsetBase);
    ReplaceChildrenFromParsedNode(document, target, parsedNode, sourceOffsetBase, mutations,
                                  mutationScope);
  }

  SyncValueFromParsed(target, parsedNode, mutations, mutationScope);
  SetSourceOffsetsFromParsed(target, parsedNode, sourceOffsetBase);
}

bool HasCompatibleNodeIdentity(const XMLNode& target, const XMLNode& parsedNode) {
  if (target.type() != parsedNode.type()) {
    return false;
  }

  switch (target.type()) {
    case XMLNode::Type::Element:
    case XMLNode::Type::ProcessingInstruction:
    case XMLNode::Type::XMLDeclaration: return target.tagName() == parsedNode.tagName();

    case XMLNode::Type::Document:
    case XMLNode::Type::Data:
    case XMLNode::Type::CData:
    case XMLNode::Type::Comment:
    case XMLNode::Type::DocType: return true;
  }

  UTILS_UNREACHABLE();
}

bool SyncSourceLocationsFromParsedByPosition(XMLNode& target, const XMLNode& parsedNode,
                                             std::size_t sourceOffsetBase) {
  if (!HasCompatibleNodeIdentity(target, parsedNode)) {
    return false;
  }

  std::vector<XMLNode> targetChildren;
  for (std::optional<XMLNode> child = target.firstChild(); child.has_value();
       child = child->nextSibling()) {
    targetChildren.push_back(*child);
  }

  std::vector<XMLNode> parsedChildren;
  for (std::optional<XMLNode> child = parsedNode.firstChild(); child.has_value();
       child = child->nextSibling()) {
    parsedChildren.push_back(*child);
  }

  if (targetChildren.size() != parsedChildren.size()) {
    return false;
  }

  if (target.type() == XMLNode::Type::Element || target.type() == XMLNode::Type::XMLDeclaration) {
    SyncAttributesFromParsed(target, parsedNode);
    SyncAttributeSourceLocationsFromParsed(target, parsedNode, sourceOffsetBase);
  }

  SyncValueFromParsed(target, parsedNode);
  SetSourceOffsetsFromParsed(target, parsedNode, sourceOffsetBase);

  for (std::size_t index = 0; index < targetChildren.size(); ++index) {
    if (!SyncSourceLocationsFromParsedByPosition(targetChildren[index], parsedChildren[index],
                                                 sourceOffsetBase)) {
      return false;
    }
  }

  return true;
}

XMLNode CloneParsedNodeInto(XMLDocument& document, const XMLNode& parsedNode,
                            std::size_t sourceOffsetBase) {
  XMLNode clone = [&]() {
    switch (parsedNode.type()) {
      case XMLNode::Type::Element:
        return XMLNode::CreateElementNode(document, parsedNode.tagName());
      case XMLNode::Type::Data:
        return XMLNode::CreateDataNode(document, parsedNode.value().value_or(RcString("")));
      case XMLNode::Type::CData:
        return XMLNode::CreateCDataNode(document, parsedNode.value().value_or(RcString("")));
      case XMLNode::Type::Comment:
        return XMLNode::CreateCommentNode(document, parsedNode.value().value_or(RcString("")));
      case XMLNode::Type::DocType:
        return XMLNode::CreateDocTypeNode(document, parsedNode.value().value_or(RcString("")));
      case XMLNode::Type::ProcessingInstruction:
        return XMLNode::CreateProcessingInstructionNode(document, parsedNode.tagName().name,
                                                        parsedNode.value().value_or(RcString("")));
      case XMLNode::Type::XMLDeclaration: return XMLNode::CreateXMLDeclarationNode(document);
      case XMLNode::Type::Document: break;
    }

    UTILS_UNREACHABLE();
  }();

  SyncNodeFromParsed(document, clone, parsedNode, sourceOffsetBase);
  return clone;
}

void ReplaceChildrenFromParsedNode(XMLDocument& document, XMLNode& target,
                                   const XMLNode& parsedTarget, std::size_t sourceOffsetBase,
                                   std::vector<XMLMutation>* mutations,
                                   ReparseScope mutationScope) {
  std::vector<XMLNode> oldChildren;
  for (std::optional<XMLNode> child = target.firstChild(); child.has_value();
       child = child->nextSibling()) {
    oldChildren.push_back(*child);
  }

  for (const XMLNode& child : oldChildren) {
    target.removeChild(child);
  }

  std::vector<bool> usedChildren(oldChildren.size(), false);
  for (std::optional<XMLNode> child = parsedTarget.firstChild(); child.has_value();
       child = child->nextSibling()) {
    std::optional<XMLNode> nodeToAppend;
    bool reusedExistingChild = false;
    if (std::optional<std::size_t> oldIndex =
            FindReusableChild(*child, oldChildren, usedChildren)) {
      usedChildren[*oldIndex] = true;
      nodeToAppend = oldChildren[*oldIndex];
      reusedExistingChild = true;
      SyncNodeFromParsed(document, *nodeToAppend, *child, sourceOffsetBase, mutations,
                         mutationScope);
    } else {
      nodeToAppend = CloneParsedNodeInto(document, *child, sourceOffsetBase);
    }

    target.appendChild(*nodeToAppend);
    if (mutations != nullptr && !reusedExistingChild) {
      mutations->push_back(XMLMutation{
          .kind = XMLMutation::Kind::NodeInserted,
          .node = *nodeToAppend,
          .attributeName = XMLQualifiedName(""),
          .value = std::nullopt,
          .scope = mutationScope,
      });
    }
  }

  for (std::size_t index = 0; index < oldChildren.size(); ++index) {
    if (!usedChildren[index]) {
      ClearSourceLocationsRecursive(oldChildren[index]);
      if (mutations != nullptr) {
        mutations->push_back(XMLMutation{
            .kind = XMLMutation::Kind::NodeRemoved,
            .node = oldChildren[index],
            .attributeName = XMLQualifiedName(""),
            .value = std::nullopt,
            .scope = mutationScope,
        });
      }
    }
  }

  SyncValueFromParsed(target, parsedTarget, mutations, mutationScope);
}

void AppendAttributeMutations(XMLNode& node, const AttributeMap& currentAttributes,
                              const AttributeMap& reparsedAttributes,
                              ApplySourceEditResult& result) {
  for (const auto& [name, value] : currentAttributes) {
    if (!reparsedAttributes.contains(name)) {
      node.removeAttribute(name);
      result.mutations.push_back(XMLMutation{
          .kind = XMLMutation::Kind::AttributeRemoved,
          .node = node,
          .attributeName = name,
          .value = std::nullopt,
          .scope = ReparseScope::OpeningTag,
      });
    }
  }

  for (const auto& [name, value] : reparsedAttributes) {
    const auto currentIt = currentAttributes.find(name);
    if (currentIt == currentAttributes.end() || currentIt->second != value) {
      node.setAttribute(name, value);
      result.mutations.push_back(XMLMutation{
          .kind = XMLMutation::Kind::AttributeSet,
          .node = node,
          .attributeName = name,
          .value = value,
          .scope = ReparseScope::OpeningTag,
      });
    }
  }
}

SourceEditClassification ClassifySourceEdit(const XMLDocument& document, SourceEditRange range) {
  SourceEditClassification classification;
  classification.attribute = GetAttributeValueEdit(document, range);
  if (classification.attribute.has_value()) {
    classification.scope = ReparseScope::AttributeValue;
  } else if ((classification.openingTag = GetOpeningTagEdit(document, range)).has_value()) {
    classification.scope = ReparseScope::OpeningTag;
  } else if ((classification.textNode = GetTextNodeEdit(document, range)).has_value()) {
    classification.scope = ReparseScope::TextNode;
  } else if ((classification.elementSubtree = GetElementSubtreeEdit(document, range)).has_value()) {
    classification.scope = ReparseScope::ElementSubtree;
  }
  return classification;
}

bool ProspectiveFragmentFits(std::size_t retainedBytes, std::size_t replacementBytes,
                             std::size_t maximumSourceSize) {
  if (replacementBytes > maximumSourceSize) {
    return false;
  }
  return retainedBytes <= maximumSourceSize - replacementBytes;
}

std::optional<std::string> BuildProspectiveFragment(std::string_view source, SourceEditRange range,
                                                    std::size_t fragmentStart,
                                                    std::size_t fragmentEnd,
                                                    std::string_view replacement,
                                                    std::size_t maximumSourceSize) {
  const std::size_t removedBytes = range.end - range.start;
  const std::size_t retainedBytes = fragmentEnd - fragmentStart - removedBytes;
  if (!ProspectiveFragmentFits(retainedBytes, replacement.size(), maximumSourceSize)) {
    return std::nullopt;
  }

  std::string prospective;
  prospective.reserve(retainedBytes + replacement.size());
  prospective.append(source.substr(fragmentStart, range.start - fragmentStart));
  prospective.append(replacement);
  prospective.append(source.substr(range.end, fragmentEnd - range.end));
  return prospective;
}

std::optional<ParseDiagnostic> IncrementalLimitParseDiagnostic(const ParseDiagnostic& diagnostic,
                                                               SourceRange range) {
  const std::string_view reason = diagnostic.reason;
  if (reason == "Maximum element count exceeded") {
    return MakeEditDiagnostic("Incremental source edit exceeds the document tree-node limit",
                              range);
  }
  if (reason == "Maximum element nesting depth exceeded") {
    return MakeEditDiagnostic("Incremental source edit exceeds the document tree-depth limit",
                              range);
  }
  if (reason == "Maximum total attribute count exceeded") {
    return MakeEditDiagnostic("Incremental source edit exceeds the document total-attribute limit",
                              range);
  }
  return std::nullopt;
}

std::optional<ParseDiagnostic> OpeningTagLimitParseDiagnostic(const ParseDiagnostic& diagnostic,
                                                              SourceRange range) {
  if (std::string_view(diagnostic.reason) == "Maximum total attribute count exceeded") {
    return MakeEditDiagnostic("Incremental source edit exceeds the document total-attribute limit",
                              range);
  }
  return std::nullopt;
}

std::optional<XMLNode> SingleMatchingElement(XMLDocument& parsedDocument,
                                             const XMLQualifiedNameRef& expectedName) {
  std::optional<XMLNode> parsedNode = parsedDocument.root().firstChild();
  if (!parsedNode.has_value() || parsedNode->type() != XMLNode::Type::Element ||
      parsedNode->nextSibling().has_value() || parsedNode->tagName() != expectedName) {
    return std::nullopt;
  }
  return parsedNode;
}

std::optional<ParseDiagnostic> PreflightOpeningTagEdit(XMLDocument& document, XMLSourceStore& store,
                                                       const OpeningTagEdit& edit,
                                                       SourceEditRange range,
                                                       const XMLEditIntent& intent) {
  std::optional<std::string> prospective =
      BuildProspectiveFragment(document.source(), range, edit.tagStart, edit.tagEnd,
                               intent.replacement, store.resourceLimits().maximumSourceSize);
  if (!prospective.has_value()) {
    return std::nullopt;
  }

  ParseResult<XMLDocument> parsed = XMLIncrementalParser::ParseOpeningTag(
      *prospective, IncrementalPreflightOptions(document, prospective->size()));
  if (parsed.hasError()) {
    return OpeningTagLimitParseDiagnostic(parsed.error(), intent.range);
  }

  std::optional<XMLNode> parsedNode = SingleMatchingElement(parsed.result(), edit.node.tagName());
  if (!parsedNode.has_value()) {
    return std::nullopt;
  }
  return ValidateOpeningTagAttributeLimit(document, edit.node, *parsedNode, intent.range);
}

std::optional<std::pair<std::size_t, std::size_t>> ElementSubtreeOffsets(
    const ElementSubtreeEdit& edit, SourceEditRange range) {
  const std::optional<SourceRange> location = edit.node.getNodeLocation();
  if (!location.has_value() || !location->start.offset.has_value() ||
      !location->end.offset.has_value() || *location->start.offset > range.start ||
      range.end > *location->end.offset) {
    return std::nullopt;
  }
  return std::pair(*location->start.offset, *location->end.offset);
}

std::optional<ParseDiagnostic> PreflightElementSubtreeEdit(XMLDocument& document,
                                                           XMLSourceStore& store,
                                                           const ElementSubtreeEdit& edit,
                                                           SourceEditRange range,
                                                           const XMLEditIntent& intent) {
  const std::optional<std::pair<std::size_t, std::size_t>> offsets =
      ElementSubtreeOffsets(edit, range);
  if (!offsets.has_value()) {
    return std::nullopt;
  }

  std::optional<std::string> prospective =
      BuildProspectiveFragment(document.source(), range, offsets->first, offsets->second,
                               intent.replacement, store.resourceLimits().maximumSourceSize);
  if (!prospective.has_value()) {
    return std::nullopt;
  }

  ParseResult<XMLDocument> parsed = XMLIncrementalParser::ParseElement(
      *prospective, IncrementalPreflightOptions(document, prospective->size()));
  if (parsed.hasError()) {
    return IncrementalLimitParseDiagnostic(parsed.error(), intent.range);
  }

  std::optional<XMLNode> parsedNode = SingleMatchingElement(parsed.result(), edit.node.tagName());
  if (!parsedNode.has_value()) {
    return std::nullopt;
  }
  return ValidateIncrementalTreeLimits(document, edit.node, *parsedNode, intent.range);
}

std::optional<ParseDiagnostic> PreflightSourceEdit(XMLDocument& document, XMLSourceStore& store,
                                                   const SourceEditClassification& classification,
                                                   SourceEditRange range,
                                                   const XMLEditIntent& intent) {
  if (classification.openingTag.has_value()) {
    return PreflightOpeningTagEdit(document, store, *classification.openingTag, range, intent);
  }
  if (classification.elementSubtree.has_value()) {
    return PreflightElementSubtreeEdit(document, store, *classification.elementSubtree, range,
                                       intent);
  }
  return std::nullopt;
}

ApplySourceEditResult FinishSourceEditWithDiagnostic(XMLDocument& document,
                                                     ApplySourceEditResult result,
                                                     ParseDiagnostic diagnostic,
                                                     const XMLNode& node) {
  result.diagnostic = std::move(diagnostic);
  UpdateSourceDiagnostic(document, result, node, result.diagnostic);
  return result;
}

ApplySourceEditResult FinishSourceEditSuccess(XMLDocument& document, ApplySourceEditResult result,
                                              const XMLNode& node) {
  UpdateSourceDiagnostic(document, result, node, std::nullopt);
  return result;
}

ApplySourceEditResult ApplyOpeningTagSourceEdit(XMLDocument& document, const XMLEditIntent& intent,
                                                const OpeningTagEdit& edit,
                                                ApplySourceEditResult result) {
  const std::optional<SourceRange> nodeLocation = edit.node.getNodeLocation();
  if (!nodeLocation.has_value() || !nodeLocation->start.offset.has_value()) {
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        MakeEditDiagnostic("Opening tag edit left the node source range unavailable", intent.range),
        edit.node);
  }

  const std::size_t tagStart = *nodeLocation->start.offset;
  const std::optional<std::size_t> tagEnd = FindOpeningTagEnd(document.source(), tagStart);
  if (!tagEnd.has_value()) {
    SourceRange dirtyRange = intent.range;
    if (nodeLocation->end.offset.has_value()) {
      dirtyRange =
          SourceRange{FileOffset::Offset(tagStart), FileOffset::Offset(*nodeLocation->end.offset)};
    }
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        MakeEditDiagnostic("Opening tag edit left the opening tag malformed", dirtyRange),
        edit.node);
  }

  ParseResult<XMLDocument> parsed =
      XMLIncrementalParser::ParseOpeningTag(document.source().substr(tagStart, *tagEnd - tagStart));
  if (parsed.hasError()) {
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        RebaseDiagnosticToDirtyRange(std::move(parsed).error(),
                                     SourceEditRange{.start = tagStart, .end = *tagEnd}),
        edit.node);
  }

  std::optional<XMLNode> parsedNode = parsed.result().root().firstChild();
  if (!parsedNode.has_value()) {
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        MakeEditDiagnostic("Opening tag edit did not produce an element", intent.range), edit.node);
  }
  if (parsedNode->tagName() != edit.node.tagName()) {
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        MakeEditDiagnostic("Opening tag element rename is not implemented", intent.range),
        edit.node);
  }

  const AttributeMap currentAttributes = BuildAttributeMap(edit.node);
  const AttributeMap reparsedAttributes = BuildAttributeMap(*parsedNode);
  XMLNode target = edit.node;
  AppendAttributeMutations(target, currentAttributes, reparsedAttributes, result);
  SyncAttributeSourceLocationsFromParsed(target, *parsedNode, tagStart);
  return FinishSourceEditSuccess(document, std::move(result), target);
}

ApplySourceEditResult ApplyRawTextSourceEdit(XMLDocument& document, const XMLEditIntent& intent,
                                             const TextNodeEdit& edit,
                                             ApplySourceEditResult result) {
  const std::optional<SourceRange> nodeLocation = edit.node.getNodeLocation();
  const std::optional<SourceEditRange> updatedRange =
      nodeLocation.has_value() ? ResolveEditRange(*nodeLocation, document.source()) : std::nullopt;
  if (!updatedRange.has_value()) {
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        MakeEditDiagnostic("Text-like node edit left the node source range unavailable",
                           intent.range),
        edit.node);
  }

  ParseResult<XMLDocument> parsed = XMLIncrementalParser::ParseTextLikeNode(
      document.source().substr(updatedRange->start, updatedRange->end - updatedRange->start));
  if (parsed.hasError()) {
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        RebaseDiagnosticToDirtyRange(std::move(parsed).error(), *updatedRange), edit.node);
  }

  std::optional<XMLNode> parsedNode = parsed.result().root().firstChild();
  if (!parsedNode.has_value() || parsedNode->type() != edit.node.type() ||
      parsedNode->nextSibling().has_value()) {
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        MakeEditDiagnostic("Text-like node edit changed the local XML structure", intent.range),
        edit.node);
  }
  if (edit.node.type() == XMLNode::Type::ProcessingInstruction &&
      parsedNode->tagName() != edit.node.tagName()) {
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        MakeEditDiagnostic("Processing instruction target rename is not implemented", intent.range),
        edit.node);
  }

  const RcString parsedValue = parsedNode->value().value_or(RcString(""));
  XMLNode target = edit.node;
  target.setValue(parsedValue);
  SetSourceOffsetsFromParsed(target, *parsedNode, updatedRange->start);
  result.mutations.push_back(XMLMutation{
      .kind = XMLMutation::Kind::NodeValueChanged,
      .node = target,
      .attributeName = XMLQualifiedName(""),
      .value = parsedValue,
      .scope = ReparseScope::TextNode,
  });
  return FinishSourceEditSuccess(document, std::move(result), target);
}

ApplySourceEditResult ApplyParsedTextSourceEdit(XMLDocument& document, const XMLEditIntent& intent,
                                                const TextNodeEdit& edit,
                                                ApplySourceEditResult result) {
  const std::optional<SourceEditRange> updatedRange = GetTextNodeSourceRange(document, edit);
  if (!updatedRange.has_value()) {
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        MakeEditDiagnostic("Text node edit left the node source range unavailable", intent.range),
        edit.node);
  }

  ParseResult<XMLDocument> parsed = XMLIncrementalParser::ParsePcdata(
      document.source().substr(updatedRange->start, updatedRange->end - updatedRange->start));
  if (parsed.hasError()) {
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        RebaseDiagnosticToDirtyRange(std::move(parsed).error(), *updatedRange), edit.node);
  }

  std::optional<XMLNode> parsedElement = parsed.result().root().firstChild();
  if (!parsedElement.has_value()) {
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        MakeEditDiagnostic("Text node edit did not produce a wrapper element", intent.range),
        edit.node);
  }
  std::optional<XMLNode> parsedTextNode = parsedElement->firstChild();
  if (!parsedTextNode.has_value() || parsedTextNode->type() != XMLNode::Type::Data ||
      parsedTextNode->nextSibling().has_value()) {
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        MakeEditDiagnostic("Text node edit changed the local XML structure", intent.range),
        edit.node);
  }

  const RcString parsedValue = parsedTextNode->value().value_or(RcString(""));
  XMLNode target = edit.node;
  target.setValue(parsedValue);
  if (!edit.elementTextContent) {
    if (std::optional<XMLNode> parent = target.parentElement()) {
      parent->setValue(parsedValue);
    }
  }
  result.mutations.push_back(XMLMutation{
      .kind = XMLMutation::Kind::NodeValueChanged,
      .node = target,
      .attributeName = XMLQualifiedName(""),
      .value = parsedValue,
      .scope = ReparseScope::TextNode,
  });
  return FinishSourceEditSuccess(document, std::move(result), target);
}

ApplySourceEditResult ApplyTextSourceEdit(XMLDocument& document, const XMLEditIntent& intent,
                                          const TextNodeEdit& edit, ApplySourceEditResult result) {
  if (edit.kind == TextNodeEditKind::RawTextLikeNode) {
    return ApplyRawTextSourceEdit(document, intent, edit, std::move(result));
  }
  return ApplyParsedTextSourceEdit(document, intent, edit, std::move(result));
}

ApplySourceEditResult ApplyElementSubtreeSourceEdit(XMLDocument& document,
                                                    const XMLEditIntent& intent,
                                                    const ElementSubtreeEdit& edit,
                                                    ApplySourceEditResult result) {
  const std::optional<SourceRange> nodeLocation = edit.node.getNodeLocation();
  if (!nodeLocation.has_value() || !nodeLocation->start.offset.has_value() ||
      !nodeLocation->end.offset.has_value()) {
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        MakeEditDiagnostic("Element subtree edit left the node source range unavailable",
                           intent.range),
        edit.node);
  }

  const std::size_t nodeStart = *nodeLocation->start.offset;
  const std::size_t nodeEnd = *nodeLocation->end.offset;
  ParseResult<XMLDocument> parsed =
      XMLIncrementalParser::ParseElement(document.source().substr(nodeStart, nodeEnd - nodeStart));
  if (parsed.hasError()) {
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        RebaseDiagnosticToDirtyRange(std::move(parsed).error(),
                                     SourceEditRange{.start = nodeStart, .end = nodeEnd}),
        edit.node);
  }

  std::optional<XMLNode> parsedNode = parsed.result().root().firstChild();
  if (!parsedNode.has_value() || parsedNode->type() != XMLNode::Type::Element ||
      parsedNode->nextSibling().has_value()) {
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        MakeEditDiagnostic("Element subtree edit did not produce one element", intent.range),
        edit.node);
  }
  if (parsedNode->tagName() != edit.node.tagName()) {
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        MakeEditDiagnostic("Element subtree edit renamed the target element", intent.range),
        edit.node);
  }

  XMLNode target = edit.node;
  std::vector<XMLMutation> subtreeMutations;
  ReplaceChildrenFromParsedNode(document, target, *parsedNode, nodeStart, &subtreeMutations,
                                ReparseScope::ElementSubtree);
  result.mutations.push_back(XMLMutation{
      .kind = XMLMutation::Kind::SubtreeReplaced,
      .node = target,
      .attributeName = XMLQualifiedName(""),
      .value = std::nullopt,
      .scope = ReparseScope::ElementSubtree,
  });
  result.mutations.insert(result.mutations.end(), subtreeMutations.begin(), subtreeMutations.end());
  return FinishSourceEditSuccess(document, std::move(result), target);
}

struct UpdatedAttributeRanges {
  std::optional<SourceEditRange> attribute;
  std::optional<SourceEditRange> value;
};

UpdatedAttributeRanges ResolveUpdatedAttributeRanges(const XMLDocument& document,
                                                     const AttributeValueEdit& edit) {
  UpdatedAttributeRanges ranges;
  if (std::optional<XMLAttributeSourceLocation> location =
          edit.node.getAttributeSourceLocation(edit.name)) {
    ranges.attribute = ResolveEditRange(location->fullRange, document.source());
    ranges.value = ResolveEditRange(location->valueRange, document.source());
  }

  const std::optional<SourceRange> location =
      edit.node.getAttributeLocation(document.source(), edit.name);
  if (!ranges.attribute.has_value() && location.has_value()) {
    ranges.attribute = ResolveEditRange(*location, document.source());
  }
  if (!ranges.value.has_value() && location.has_value()) {
    const std::optional<AttributeValueEdit> value =
        GetAttributeValueSpan(edit.node, edit.name, *location, document.source());
    if (value.has_value()) {
      ranges.value = SourceEditRange{.start = value->valueStart, .end = value->valueEnd};
    }
  }
  return ranges;
}

ApplySourceEditResult ApplyAttributeValueSourceEdit(XMLDocument& document,
                                                    const XMLEditIntent& intent,
                                                    const AttributeValueEdit& edit,
                                                    ApplySourceEditResult result) {
  const UpdatedAttributeRanges ranges = ResolveUpdatedAttributeRanges(document, edit);
  if (!ranges.attribute.has_value()) {
    const SourceRange diagnosticRange =
        ranges.value.has_value() ? ToSourceRange(*ranges.value) : intent.range;
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        MakeEditDiagnostic("Attribute value edit left the opening tag malformed", diagnosticRange),
        edit.node);
  }

  ParseResult<XMLDocument> parsed = XMLIncrementalParser::ParseAttribute(document.source().substr(
      ranges.attribute->start, ranges.attribute->end - ranges.attribute->start));
  if (parsed.hasError()) {
    const SourceEditRange diagnosticRange =
        ranges.value.has_value() ? *ranges.value : *ranges.attribute;
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        RebaseDiagnosticToDirtyRange(std::move(parsed).error(), diagnosticRange), edit.node);
  }

  std::optional<XMLNode> parsedNode = parsed.result().root().firstChild();
  if (!parsedNode.has_value()) {
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        MakeEditDiagnostic("Attribute value edit did not produce an element",
                           ToSourceRange(*ranges.attribute)),
        edit.node);
  }
  std::optional<RcString> parsedValue = parsedNode->getAttribute(edit.name);
  if (!parsedValue.has_value()) {
    return FinishSourceEditWithDiagnostic(
        document, std::move(result),
        MakeEditDiagnostic("Attribute value edit removed the target attribute",
                           ToSourceRange(*ranges.attribute)),
        edit.node);
  }

  XMLNode target = edit.node;
  target.setAttribute(edit.name, *parsedValue);
  RefreshAttributeSourceLocation(document, target, edit.name);
  result.mutations.push_back(XMLMutation{
      .kind = XMLMutation::Kind::AttributeSet,
      .node = target,
      .attributeName = edit.name,
      .value = *parsedValue,
      .scope = ReparseScope::AttributeValue,
  });
  return FinishSourceEditSuccess(document, std::move(result), target);
}

ApplySourceEditResult ApplyClassifiedSourceEdit(XMLDocument& document, const XMLEditIntent& intent,
                                                const SourceEditClassification& classification,
                                                ApplySourceEditResult result) {
  if (classification.attribute.has_value()) {
    return ApplyAttributeValueSourceEdit(document, intent, *classification.attribute,
                                         std::move(result));
  }
  if (classification.openingTag.has_value()) {
    return ApplyOpeningTagSourceEdit(document, intent, *classification.openingTag,
                                     std::move(result));
  }
  if (classification.textNode.has_value()) {
    return ApplyTextSourceEdit(document, intent, *classification.textNode, std::move(result));
  }
  if (classification.elementSubtree.has_value()) {
    return ApplyElementSubtreeSourceEdit(document, intent, *classification.elementSubtree,
                                         std::move(result));
  }

  result.diagnostic = MakeEditDiagnostic(
      "Only attribute-value, opening-tag, text-node, and element-subtree source edits are "
      "implemented",
      intent.range);
  return result;
}

}  // namespace internal

using namespace internal;

std::ostream& operator<<(std::ostream& os, ReparseScope scope) {
  switch (scope) {
    case ReparseScope::AttributeValue: return os << "AttributeValue";
    case ReparseScope::OpeningTag: return os << "OpeningTag";
    case ReparseScope::TextNode: return os << "TextNode";
    case ReparseScope::ElementSubtree: return os << "ElementSubtree";
    case ReparseScope::Document: return os << "Document";
  }

  UTILS_UNREACHABLE();
}

XMLDocument::XMLDocument() : registry_(std::make_shared<Registry>()) {
  // Tree mutations always go through TreeMutationContext; install the basic XML defaults so
  // higher-level models (SVGDocument) can just override individual callbacks. Done before
  // XMLDocumentContext / XMLNamespaceContext so `XMLNode::CreateDocumentNode` below sees a
  // fully-installed mutation context.
  registry_->ctx().emplace<donner::components::TreeMutationContext>();

  auto& ctx = registry_->ctx().emplace<XMLDocumentContext>(XMLDocumentContext::InternalCtorTag{});
  ctx.rootEntity = XMLNode::CreateDocumentNode(*this).entityHandle().entity();

  registry_->ctx().emplace<XMLNamespaceContext>(*registry_);
}

XMLDocument::XMLDocument(std::shared_ptr<Registry> registry) : registry_(std::move(registry)) {
  // A shared registry may already have a TreeMutationContext installed (e.g. by a higher-level
  // document model that owns the registry); install the basic XML defaults only if not.
  if (!registry_->ctx().contains<donner::components::TreeMutationContext>()) {
    registry_->ctx().emplace<donner::components::TreeMutationContext>();
  }
}

XMLDocument XMLDocument::CreateFromRegistry(std::shared_ptr<Registry> registry) {
  UTILS_RELEASE_ASSERT_MSG(registry != nullptr, "Cannot create XMLDocument from null registry");
  UTILS_RELEASE_ASSERT_MSG(registry->ctx().contains<XMLDocumentContext>(),
                           "Registry does not contain XMLDocumentContext");
  return XMLDocument(std::move(registry));
}

XMLNode XMLDocument::root() const {
  return XMLNode(rootEntityHandle());
}

EntityHandle XMLDocument::rootEntityHandle() const {
  return EntityHandle(*registry_, registry_->ctx().get<XMLDocumentContext>().rootEntity);
}

bool XMLDocument::hasSourceStore() const {
  return registry_->ctx().get<XMLDocumentContext>().sourceStore != nullptr;
}

std::string_view XMLDocument::source() const {
  const XMLSourceStore* store = sourceStore();
  return store != nullptr ? store->source() : std::string_view();
}

std::uint64_t XMLDocument::sourceVersion() const {
  const XMLSourceStore* store = sourceStore();
  return store != nullptr ? store->sourceVersion() : 0;
}

XMLSourceStore* XMLDocument::sourceStore() {
  return registry_->ctx().get<XMLDocumentContext>().sourceStore.get();
}

const XMLSourceStore* XMLDocument::sourceStore() const {
  return registry_->ctx().get<XMLDocumentContext>().sourceStore.get();
}

void XMLDocument::setSourceEditTreeLimits(std::size_t maximumTreeNodes,
                                          std::size_t maximumTreeDepth) {
  auto& context = registry_->ctx().get<XMLDocumentContext>();
  context.maximumSourceEditTreeNodes = static_cast<std::uint64_t>(maximumTreeNodes);
  context.maximumSourceEditTreeDepth = static_cast<int>(
      std::min(maximumTreeDepth, static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

std::optional<XMLNode> XMLDocument::nodeAtSourceOffset(std::size_t offset) const {
  if (!hasSourceStore() || offset >= source().size()) {
    return std::nullopt;
  }

  SourceIntervalIndex index;
  BuildSourceIntervalIndex(root(), 0, index);
  return LookupSourceIntervalIndex(index, offset);
}

std::optional<XMLAttributeAtSourceOffset> XMLDocument::attributeAtSourceOffset(
    std::size_t offset) const {
  if (!hasSourceStore() || offset >= source().size()) {
    return std::nullopt;
  }

  std::optional<XMLNode> node = nodeAtSourceOffset(offset);
  if (!node.has_value()) {
    return std::nullopt;
  }

  for (const XMLQualifiedNameRef& name : node->attributes()) {
    std::optional<XMLAttributeSourceLocation> sourceLocation =
        node->getAttributeSourceLocation(name);
    if (sourceLocation.has_value() && ContainsOffset(sourceLocation->fullRange, offset)) {
      return XMLAttributeAtSourceOffset{
          .node = *node,
          .name = XMLQualifiedName(RcString(name.namespacePrefix), RcString(name.name)),
          .location = sourceLocation->fullRange,
          .valueLocation = sourceLocation->valueRange,
          .quote = sourceLocation->quote,
      };
    }

    std::optional<SourceRange> location = node->getAttributeLocation(source(), name);
    std::optional<AttributeValueEdit> valueEdit =
        location.has_value()
            ? GetAttributeValueSpan(
                  *node, XMLQualifiedName(RcString(name.namespacePrefix), RcString(name.name)),
                  *location, source())
            : std::nullopt;
    if (location.has_value() && valueEdit.has_value() && ContainsOffset(*location, offset)) {
      return XMLAttributeAtSourceOffset{
          .node = *node,
          .name = XMLQualifiedName(RcString(name.namespacePrefix), RcString(name.name)),
          .location = *location,
          .valueLocation = SourceRange{FileOffset::Offset(valueEdit->valueStart),
                                       FileOffset::Offset(valueEdit->valueEnd)},
          .quote = valueEdit->quote,
      };
    }
  }

  return std::nullopt;
}

ApplySourceEditResult XMLDocument::applySourceEdit(const XMLEditIntent& intent) {
  ApplySourceEditResult result;
  result.scope = ReparseScope::Document;

  XMLSourceStore* store = sourceStore();
  if (store == nullptr) {
    result.diagnostic = MakeEditDiagnostic(
        "Cannot apply source edit to a document without source text", intent.range);
    return result;
  }
  if (intent.sourceVersion != sourceVersion()) {
    result.diagnostic = MakeEditDiagnostic("Source version mismatch", intent.range);
    return result;
  }

  const std::optional<SourceEditRange> range = ResolveEditRange(intent.range, source());
  if (!range.has_value()) {
    result.diagnostic = MakeEditDiagnostic("Invalid source edit range", intent.range);
    return result;
  }

  const SourceEditClassification classification = ClassifySourceEdit(*this, *range);
  result.scope = classification.scope;
  if (std::optional<ParseDiagnostic> diagnostic =
          PreflightSourceEdit(*this, *store, classification, *range, intent)) {
    result.diagnostic = std::move(diagnostic);
    return result;
  }

  const std::optional<XMLSourceDelta> delta =
      store->replace(range->start, range->end - range->start, intent.replacement);
  if (!delta.has_value()) {
    result.diagnostic = MakeEditDiagnostic("Invalid source replacement", intent.range);
    return result;
  }

  result.applied = true;
  result.sourceDeltas.push_back(*delta);
  return ApplyClassifiedSourceEdit(*this, intent, classification, std::move(result));
}

ApplySourceEditResult XMLDocument::setAttribute(XMLNode node, const XMLQualifiedNameRef& name,
                                                std::string_view value) {
  ApplySourceEditResult result;
  result.scope = ReparseScope::AttributeValue;

  const XMLQualifiedName ownedName = MakeOwnedName(name);
  const SourceRange diagnosticRange = MakeNodeDiagnosticRange(node);
  if (!IsDocumentNode(*this, node)) {
    result.diagnostic =
        MakeEditDiagnostic("Cannot set attribute on a node from another document", diagnosticRange);
    return result;
  }

  XMLSourceStore* store = sourceStore();
  if (store == nullptr) {
    result.diagnostic = MakeEditDiagnostic("Cannot set source-backed attribute without source text",
                                           diagnosticRange);
    return result;
  }

  if (node.type() != XMLNode::Type::Element && node.type() != XMLNode::Type::XMLDeclaration) {
    result.diagnostic = MakeEditDiagnostic(
        "Cannot set attribute on a node that does not support attributes", diagnosticRange);
    return result;
  }

  std::optional<AttributeValueEdit> edit = GetAttributeValueEditForNode(*this, node, ownedName);
  if (!edit.has_value()) {
    result.scope = ReparseScope::OpeningTag;

    if (node.hasAttribute(ownedName)) {
      result.diagnostic =
          MakeEditDiagnostic("Cannot update attribute without a source range", diagnosticRange);
      return result;
    }

    std::optional<std::size_t> insertionOffset = GetAttributeInsertionOffset(*this, node);
    if (!insertionOffset.has_value()) {
      result.diagnostic = MakeEditDiagnostic(
          "Cannot insert attribute without an opening tag source range", diagnosticRange);
      return result;
    }

    std::optional<RcString> escapedValue = EscapeAttributeValue(value, '"');
    if (!escapedValue.has_value()) {
      result.diagnostic = MakeEditDiagnostic("Attribute value cannot be represented in XML source",
                                             diagnosticRange);
      return result;
    }

    const std::string insertion =
        SerializeAttributeInsertion(source(), *insertionOffset, ownedName, *escapedValue);
    std::optional<XMLSourceDelta> delta = store->replace(*insertionOffset, 0, insertion);
    if (!delta.has_value()) {
      result.diagnostic =
          MakeEditDiagnostic("Invalid source replacement for attribute insertion", diagnosticRange);
      return result;
    }

    result.applied = true;
    result.sourceDeltas.push_back(*delta);

    const RcString ownedValue(value);
    node.setAttribute(ownedName, ownedValue);
    RefreshAttributeSourceLocation(*this, node, ownedName);
    result.mutations.push_back(XMLMutation{
        .kind = XMLMutation::Kind::AttributeSet,
        .node = node,
        .attributeName = ownedName,
        .value = ownedValue,
        .scope = ReparseScope::OpeningTag,
    });
    return result;
  }

  std::optional<RcString> escapedValue = EscapeAttributeValue(value, edit->quote);
  if (!escapedValue.has_value()) {
    result.diagnostic = MakeEditDiagnostic("Attribute value cannot be represented in XML source",
                                           edit->attributeLocation);
    return result;
  }

  std::optional<XMLSourceDelta> delta =
      store->replace(edit->valueStart, edit->valueEnd - edit->valueStart, *escapedValue);
  if (!delta.has_value()) {
    result.diagnostic = MakeEditDiagnostic("Invalid source replacement for attribute value",
                                           edit->attributeLocation);
    return result;
  }

  result.applied = true;
  result.sourceDeltas.push_back(*delta);

  const RcString ownedValue(value);
  node.setAttribute(ownedName, ownedValue);
  RefreshAttributeSourceLocation(*this, node, ownedName);
  result.mutations.push_back(XMLMutation{
      .kind = XMLMutation::Kind::AttributeSet,
      .node = node,
      .attributeName = ownedName,
      .value = ownedValue,
      .scope = ReparseScope::AttributeValue,
  });
  return result;
}

ApplySourceEditResult XMLDocument::removeAttribute(XMLNode node, const XMLQualifiedNameRef& name) {
  ApplySourceEditResult result;
  result.scope = ReparseScope::OpeningTag;

  const XMLQualifiedName ownedName = MakeOwnedName(name);
  const SourceRange diagnosticRange = MakeNodeDiagnosticRange(node);
  if (!IsDocumentNode(*this, node)) {
    result.diagnostic = MakeEditDiagnostic(
        "Cannot remove attribute from a node in another document", diagnosticRange);
    return result;
  }

  XMLSourceStore* store = sourceStore();
  if (store == nullptr) {
    result.diagnostic = MakeEditDiagnostic(
        "Cannot remove source-backed attribute without source text", diagnosticRange);
    return result;
  }

  if (!node.hasAttribute(ownedName)) {
    return result;
  }

  std::optional<SourceEditRange> removalRange = GetAttributeRemovalRange(*this, node, ownedName);
  if (!removalRange.has_value()) {
    result.diagnostic =
        MakeEditDiagnostic("Cannot remove attribute without a source range", diagnosticRange);
    return result;
  }

  std::optional<XMLSourceDelta> delta = store->replace(
      removalRange->start, removalRange->end - removalRange->start, std::string_view());
  if (!delta.has_value()) {
    result.diagnostic =
        MakeEditDiagnostic("Invalid source replacement for attribute removal", diagnosticRange);
    return result;
  }

  result.applied = true;
  result.sourceDeltas.push_back(*delta);

  node.removeAttribute(ownedName);
  result.mutations.push_back(XMLMutation{
      .kind = XMLMutation::Kind::AttributeRemoved,
      .node = node,
      .attributeName = ownedName,
      .value = std::nullopt,
      .scope = ReparseScope::OpeningTag,
  });
  return result;
}

ApplySourceEditResult XMLDocument::insertNode(XMLNode parent, XMLNode node,
                                              std::optional<XMLNode> referenceNode) {
  ApplySourceEditResult result;
  result.scope = ReparseScope::ElementSubtree;

  const SourceRange diagnosticRange = MakeNodeDiagnosticRange(parent);
  if (!IsDocumentNode(*this, parent) || !IsDocumentNode(*this, node) ||
      (referenceNode.has_value() && !IsDocumentNode(*this, *referenceNode))) {
    result.diagnostic =
        MakeEditDiagnostic("Cannot insert a node from another document", diagnosticRange);
    return result;
  }

  XMLSourceStore* store = sourceStore();
  if (store == nullptr) {
    result.diagnostic =
        MakeEditDiagnostic("Cannot insert source-backed node without source text", diagnosticRange);
    return result;
  }

  if (parent.type() != XMLNode::Type::Element) {
    result.diagnostic = MakeEditDiagnostic("Cannot insert under a non-element node",
                                           MakeNodeDiagnosticRange(parent));
    return result;
  }

  if (node.type() == XMLNode::Type::Document) {
    result.diagnostic =
        MakeEditDiagnostic("Cannot insert XML document root", MakeNodeDiagnosticRange(node));
    return result;
  }

  std::optional<XMLNode> currentParent = node.parentElement();
  std::optional<SourceRange> existingNodeLocation = node.getNodeLocation();
  if (currentParent.has_value() || existingNodeLocation.has_value()) {
    if (!currentParent.has_value() || !existingNodeLocation.has_value()) {
      result.diagnostic = MakeEditDiagnostic("Cannot move a partially source-backed node",
                                             MakeNodeDiagnosticRange(node));
      return result;
    }

    if (node.type() != XMLNode::Type::Element) {
      result.diagnostic = MakeEditDiagnostic("Moving non-element nodes is not implemented",
                                             MakeNodeDiagnosticRange(node));
      return result;
    }

    if (referenceNode.has_value() && *referenceNode == node) {
      return result;
    }

    std::optional<XMLNode> nextSibling = node.nextSibling();
    if (*currentParent == parent && ((!referenceNode.has_value() && !nextSibling.has_value()) ||
                                     (referenceNode.has_value() && nextSibling.has_value() &&
                                      *nextSibling == *referenceNode))) {
      return result;
    }

    if (IsAncestorOf(node, parent)) {
      result.diagnostic = MakeEditDiagnostic("Cannot move a node into its descendant",
                                             MakeNodeDiagnosticRange(node));
      return result;
    }

    std::optional<SourceEditRange> removalRange = ResolveEditRange(*existingNodeLocation, source());
    if (!removalRange.has_value()) {
      result.diagnostic = MakeEditDiagnostic("Cannot move node without a source range",
                                             MakeNodeDiagnosticRange(node));
      return result;
    }

    const std::string serialized(
        source().substr(removalRange->start, removalRange->end - removalRange->start));

    // Absorb the newline and indentation that put the node on its own line so moving it out
    // does not leave an orphaned, over-indented blank line behind. The serialized bytes above
    // are the node itself; the expanded range is only what gets deleted from the old spot.
    SourceEditRange effectiveRemoval = *removalRange;
    if (std::optional<std::string_view> nodeIndent =
            LineIndentBefore(source(), removalRange->start);
        nodeIndent.has_value()) {
      const std::size_t lineStart = removalRange->start - nodeIndent->size();
      if (lineStart > 0 && source()[lineStart - 1] == '\n') {
        effectiveRemoval.start = lineStart - 1;
      }
    }
    const std::size_t effectiveRemovalLength = effectiveRemoval.end - effectiveRemoval.start;

    std::optional<NodeInsertionPlan> insertionPlan = GetNodeInsertionPlan(
        *this, parent, referenceNode, serialized, InsertionFormatting::kStructural);
    if (!insertionPlan.has_value()) {
      result.diagnostic =
          MakeEditDiagnostic("Cannot move node without a source insertion point", diagnosticRange);
      return result;
    }

    if (removalRange->start < insertionPlan->replacementOffset &&
        insertionPlan->replacementOffset < removalRange->end) {
      result.diagnostic =
          MakeEditDiagnostic("Cannot move a node inside its own source range", diagnosticRange);
      return result;
    }

    ParseResult<XMLDocument> parsedMoved = XMLIncrementalParser::ParseElement(serialized);
    if (parsedMoved.hasError()) {
      result.diagnostic =
          RebaseDiagnosticToDirtyRange(std::move(parsedMoved).error(), *removalRange);
      return result;
    }

    std::optional<XMLNode> parsedNode = parsedMoved.result().root().firstChild();
    if (!parsedNode.has_value()) {
      result.diagnostic = MakeEditDiagnostic("Moved source did not produce an element",
                                             MakeNodeDiagnosticRange(node));
      return result;
    }

    NodeInsertionPlan appliedInsertionPlan = *insertionPlan;
    const std::string replacement =
        appliedInsertionPlan.prefix + serialized + appliedInsertionPlan.suffix;
    if (insertionPlan->replacementOffset < removalRange->start) {
      std::optional<XMLSourceDelta> insertDelta =
          store->replace(appliedInsertionPlan.replacementOffset,
                         appliedInsertionPlan.replacementLength, replacement);
      if (!insertDelta.has_value()) {
        result.diagnostic =
            MakeEditDiagnostic("Invalid source replacement for node move", diagnosticRange);
        return result;
      }
      result.sourceDeltas.push_back(*insertDelta);

      const std::size_t replacementNetLength =
          replacement.size() - appliedInsertionPlan.replacementLength;
      std::optional<XMLSourceDelta> removeDelta =
          store->replace(effectiveRemoval.start + replacementNetLength, effectiveRemovalLength,
                         std::string_view());
      if (!removeDelta.has_value()) {
        result.diagnostic =
            MakeEditDiagnostic("Invalid source removal for node move", diagnosticRange);
        return result;
      }
      result.sourceDeltas.push_back(*removeDelta);
    } else {
      std::optional<XMLSourceDelta> removeDelta =
          store->replace(effectiveRemoval.start, effectiveRemovalLength, std::string_view());
      if (!removeDelta.has_value()) {
        result.diagnostic =
            MakeEditDiagnostic("Invalid source removal for node move", diagnosticRange);
        return result;
      }
      result.sourceDeltas.push_back(*removeDelta);

      if (!ShiftPlanLeft(appliedInsertionPlan, effectiveRemovalLength)) {
        result.diagnostic =
            MakeEditDiagnostic("Invalid source insertion plan for node move", diagnosticRange);
        return result;
      }

      std::optional<XMLSourceDelta> insertDelta =
          store->replace(appliedInsertionPlan.replacementOffset,
                         appliedInsertionPlan.replacementLength, replacement);
      if (!insertDelta.has_value()) {
        result.diagnostic =
            MakeEditDiagnostic("Invalid source replacement for node move", diagnosticRange);
        return result;
      }
      result.sourceDeltas.push_back(*insertDelta);
    }

    result.applied = true;
    ClearSourceLocationsRecursive(node);
    ApplyParentInsertionPlan(parent, appliedInsertionPlan);
    if (!SyncSourceLocationsFromParsedByPosition(node, *parsedNode,
                                                 appliedInsertionPlan.insertedNodeOffset)) {
      SyncNodeFromParsed(*this, node, *parsedNode, appliedInsertionPlan.insertedNodeOffset);
    }
    parent.insertBefore(node, referenceNode);
    result.mutations.push_back(XMLMutation{
        .kind = XMLMutation::Kind::NodeRemoved,
        .node = node,
        .attributeName = XMLQualifiedName(""),
        .value = std::nullopt,
        .scope = ReparseScope::ElementSubtree,
    });
    result.mutations.push_back(XMLMutation{
        .kind = XMLMutation::Kind::NodeInserted,
        .node = node,
        .attributeName = XMLQualifiedName(""),
        .value = std::nullopt,
        .scope = ReparseScope::ElementSubtree,
    });
    return result;
  }

  const std::string serialized(std::string_view(node.serializeToString(0, false)));
  std::optional<NodeInsertionPlan> insertionPlan = GetNodeInsertionPlan(
      *this, parent, referenceNode, serialized, InsertionFormatting::kStructural);
  if (!insertionPlan.has_value()) {
    result.diagnostic =
        MakeEditDiagnostic("Cannot insert node without a source insertion point", diagnosticRange);
    return result;
  }

  ParseResult<XMLDocument> parsedInserted = XMLIncrementalParser::ParseElement(serialized);
  if (parsedInserted.hasError()) {
    result.diagnostic = RebaseDiagnosticToDirtyRange(std::move(parsedInserted).error(),
                                                     SourceEditRange{
                                                         .start = insertionPlan->insertedNodeOffset,
                                                         .end = insertionPlan->insertedNodeOffset,
                                                     });
    return result;
  }

  std::optional<XMLNode> parsedNode = parsedInserted.result().root().firstChild();
  if (!parsedNode.has_value()) {
    result.diagnostic =
        MakeEditDiagnostic("Serialized node did not produce an element", diagnosticRange);
    return result;
  }

  const std::string replacement = insertionPlan->prefix + serialized + insertionPlan->suffix;
  std::optional<XMLSourceDelta> delta = store->replace(
      insertionPlan->replacementOffset, insertionPlan->replacementLength, replacement);
  if (!delta.has_value()) {
    result.diagnostic =
        MakeEditDiagnostic("Invalid source replacement for node insertion", diagnosticRange);
    return result;
  }

  result.applied = true;
  result.sourceDeltas.push_back(*delta);

  ApplyParentInsertionPlan(parent, *insertionPlan);

  if (!SyncSourceLocationsFromParsedByPosition(node, *parsedNode,
                                               insertionPlan->insertedNodeOffset)) {
    SyncNodeFromParsed(*this, node, *parsedNode, insertionPlan->insertedNodeOffset);
  }
  parent.insertBefore(node, referenceNode);
  result.mutations.push_back(XMLMutation{
      .kind = XMLMutation::Kind::NodeInserted,
      .node = node,
      .attributeName = XMLQualifiedName(""),
      .value = std::nullopt,
      .scope = ReparseScope::ElementSubtree,
  });
  return result;
}

ApplySourceEditResult XMLDocument::removeNode(XMLNode node) {
  ApplySourceEditResult result;
  result.scope = ReparseScope::ElementSubtree;

  const SourceRange diagnosticRange = MakeNodeDiagnosticRange(node);
  if (!IsDocumentNode(*this, node)) {
    result.diagnostic =
        MakeEditDiagnostic("Cannot remove a node from another document", diagnosticRange);
    return result;
  }

  XMLSourceStore* store = sourceStore();
  if (store == nullptr) {
    result.diagnostic =
        MakeEditDiagnostic("Cannot remove source-backed node without source text", diagnosticRange);
    return result;
  }

  if (node.type() == XMLNode::Type::Document) {
    result.diagnostic = MakeEditDiagnostic("Cannot remove XML document root", diagnosticRange);
    return result;
  }

  std::optional<SourceRange> nodeLocation = node.getNodeLocation();
  std::optional<SourceEditRange> removalRange =
      nodeLocation.has_value() ? ResolveEditRange(*nodeLocation, source()) : std::nullopt;
  if (!removalRange.has_value()) {
    result.diagnostic =
        MakeEditDiagnostic("Cannot remove node without a source range", diagnosticRange);
    return result;
  }

  std::optional<XMLSourceDelta> delta = store->replace(
      removalRange->start, removalRange->end - removalRange->start, std::string_view());
  if (!delta.has_value()) {
    result.diagnostic =
        MakeEditDiagnostic("Invalid source replacement for node removal", diagnosticRange);
    return result;
  }

  result.applied = true;
  result.sourceDeltas.push_back(*delta);

  ClearSourceLocationsRecursive(node);
  node.remove();
  result.mutations.push_back(XMLMutation{
      .kind = XMLMutation::Kind::NodeRemoved,
      .node = node,
      .attributeName = XMLQualifiedName(""),
      .value = std::nullopt,
      .scope = ReparseScope::ElementSubtree,
  });
  return result;
}

ApplySourceEditResult XMLDocument::setElementText(XMLNode element, std::string_view text) {
  ApplySourceEditResult result;
  result.scope = ReparseScope::TextNode;

  const SourceRange diagnosticRange = MakeNodeDiagnosticRange(element);
  if (!IsDocumentNode(*this, element)) {
    result.diagnostic =
        MakeEditDiagnostic("Cannot set text on a node from another document", diagnosticRange);
    return result;
  }

  XMLSourceStore* store = sourceStore();
  if (store == nullptr) {
    result.diagnostic =
        MakeEditDiagnostic("Cannot set text on a node without source text", diagnosticRange);
    return result;
  }

  if (element.type() != XMLNode::Type::Element) {
    result.diagnostic =
        MakeEditDiagnostic("Cannot set text on a non-element node", diagnosticRange);
    return result;
  }

  const std::optional<RcString> escaped = EscapeTextContent(text);
  if (!escaped.has_value()) {
    result.diagnostic = MakeEditDiagnostic("Text content cannot be represented in well-formed XML",
                                           diagnosticRange);
    return result;
  }

  const auto emitValueChanged = [&]() {
    result.mutations.push_back(XMLMutation{
        .kind = XMLMutation::Kind::NodeValueChanged,
        .node = element,
        .attributeName = XMLQualifiedName(""),
        .value = RcString(text),
        .scope = ReparseScope::TextNode,
    });
  };

  // Existing text content: replace the tracked value range in place.
  if (std::optional<SourceRange> valueLocation = element.getValueLocation();
      valueLocation.has_value()) {
    std::optional<SourceEditRange> valueRange = ResolveEditRange(*valueLocation, source());
    if (!valueRange.has_value()) {
      result.diagnostic =
          MakeEditDiagnostic("Cannot set text on a node without a value range", diagnosticRange);
      return result;
    }

    std::optional<XMLSourceDelta> delta = store->replace(
        valueRange->start, valueRange->end - valueRange->start, std::string_view(*escaped));
    if (!delta.has_value()) {
      result.diagnostic =
          MakeEditDiagnostic("Invalid source replacement for text content", diagnosticRange);
      return result;
    }

    result.applied = true;
    result.sourceDeltas.push_back(*delta);
    element.setValue(RcString(text));
    if (text.empty()) {
      element.clearValueLocation();
    } else {
      element.setValueLocation(
          SourceRange{FileOffset::Offset(valueRange->start),
                      FileOffset::Offset(valueRange->start + escaped->size())});
    }
    emitValueChanged();
    return result;
  }

  // No existing text. Clearing is a no-op; otherwise insert before the closing
  // tag, expanding a self-closing tag when needed.
  if (text.empty()) {
    return result;
  }

  // Text content stays inline (`<text>hello</text>`) regardless of the surrounding layout;
  // that is how a human authors a text run, so no block indentation is synthesized here.
  std::optional<NodeInsertionPlan> plan = GetNodeInsertionPlan(
      *this, element, std::nullopt, std::string_view(*escaped), InsertionFormatting::kInline);
  if (!plan.has_value()) {
    result.diagnostic = MakeEditDiagnostic(
        "Cannot set text on a node without a source insertion point", diagnosticRange);
    return result;
  }

  const std::string replacement = plan->prefix + std::string(*escaped) + plan->suffix;
  std::optional<XMLSourceDelta> delta =
      store->replace(plan->replacementOffset, plan->replacementLength, replacement);
  if (!delta.has_value()) {
    result.diagnostic =
        MakeEditDiagnostic("Invalid source replacement for text content", diagnosticRange);
    return result;
  }

  result.applied = true;
  result.sourceDeltas.push_back(*delta);
  ApplyParentInsertionPlan(element, *plan);
  element.setValue(RcString(text));
  element.setValueLocation(SourceRange{
      FileOffset::Offset(plan->insertedNodeOffset),
      FileOffset::Offset(plan->insertedNodeOffset + escaped->size()),
  });
  emitValueChanged();
  return result;
}

void XMLDocument::setSource(std::string source, std::size_t maximumSourceSize) {
  XMLDocumentContext& context = registry_->ctx().get<XMLDocumentContext>();
  context.sourceStore = std::make_shared<XMLSourceStore>(std::move(source), maximumSourceSize);
  context.sourceDiagnostic.reset();
}

}  // namespace donner::xml
