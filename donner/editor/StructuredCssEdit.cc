#include "donner/editor/StructuredCssEdit.h"

#include <algorithm>

#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLEscape.h"
#include "donner/base/xml/XMLTokenizer.h"
#include "donner/css/parser/DeclarationListParser.h"
#include "donner/editor/DocumentSyncController.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/SourceSync.h"
#include "donner/editor/TextEditor.h"
#include "donner/editor/UndoTimeline.h"
#include "donner/svg/SVGStyleQuery.h"

namespace donner::editor {
namespace {

bool MatchesDocumentSource(std::string_view editor, std::string_view document) {
  return editor == document || (editor.size() == document.size() + 1 && editor.back() == '\n' &&
                                editor.substr(0, document.size()) == document);
}

std::optional<SourceByteRange> ByteRange(const std::optional<SourceRange>& range,
                                         std::size_t sourceBytes) {
  if (!range || !range->start.offset || !range->end.offset ||
      *range->start.offset > *range->end.offset || *range->end.offset > sourceBytes) {
    return std::nullopt;
  }
  return SourceByteRange{*range->start.offset, *range->end.offset};
}

bool Contains(SourceByteRange range, std::size_t offset) {
  return range.start <= offset && offset < range.end;
}

bool AppendDecoded(std::string& destination, std::string_view decoded,
                   const std::optional<SourceRange>& range, char suffix = '\0') {
  const auto offsets = ByteRange(range, decoded.size());
  if (!offsets) {
    return false;
  }
  destination = decoded.substr(offsets->start, offsets->end - offsets->start);
  if (suffix) {
    destination += suffix;
  }
  return true;
}

StructuredCssSourceEncoding EmbeddedEncoding(const svg::SVGStyleElement& stylesheet,
                                             std::string_view source, SourceByteRange item) {
  const auto node = xml::XMLNode::TryCast(stylesheet.entityHandle());
  const auto owner = node ? ByteRange(node->getNodeLocation(), source.size()) : std::nullopt;
  if (!owner || item.start < owner->start || item.end > owner->end) {
    return StructuredCssSourceEncoding::Unmapped;
  }
  const auto raw = source.substr(owner->start, owner->end - owner->start);
  bool cdata = false;
  xml::Tokenize(raw, [&](const xml::XMLToken& token) {
    const auto range = ByteRange(token.range, raw.size());
    if (token.type == xml::XMLTokenType::CData && range) {
      cdata |= range->start <= item.start - owner->start && item.end - owner->start <= range->end;
    }
  });
  return cdata ? StructuredCssSourceEncoding::CData : StructuredCssSourceEncoding::Text;
}

template <typename Mapper>
void AddDeclarationTarget(const css::Declaration& declaration, std::string_view decoded,
                          std::string_view source, std::size_t offset, StructuredCssTarget& target,
                          Mapper map) {
  const auto extent = map(declaration.authoredDeclarationRange());
  if (!extent) {
    return;
  }
  SourceByteRange removable = *extent;
  if (removable.end < target.insertDeclaration && source[removable.end] == ';') {
    ++removable.end;
  }
  target.declarationNeedsSeparator = removable.end == extent->end;
  if (Contains(removable, offset)) {
    const char semicolon = removable.end != extent->end ? ';' : '\0';
    if (!AppendDecoded(target.decodedMoveText, decoded, declaration.authoredDeclarationRange(),
                       semicolon)) {
      target.encoding = StructuredCssSourceEncoding::Unmapped;
    }
    target.declaration = removable;
    target.name = map(declaration.authoredNameRange());
    target.value = map(declaration.authoredValueRange());
    target.priority = map(declaration.authoredPriorityRange());
  }
}

void AddDeclarationTargets(const svg::SVGStylesheetRule& rule, std::string_view source,
                           std::size_t offset, StructuredCssTarget& target) {
  if (!rule.stylesheetElement) {
    return;
  }
  const auto map = [&](const std::optional<SourceRange>& range) -> std::optional<SourceByteRange> {
    return range ? ByteRange(svg::MapStylesheetSourceRange(*rule.stylesheetElement, *range),
                             source.size())
                 : std::nullopt;
  };
  const auto decoded = rule.stylesheetElement->textContent();
  for (const auto& entry : rule.declarations) {
    AddDeclarationTarget(entry.declaration, decoded, source, offset, target, map);
  }
}

std::optional<StructuredCssTarget> MakeEmbeddedTarget(const svg::SVGStylesheetRule& stylesheetRule,
                                                      std::string_view source, std::size_t offset,
                                                      std::uint64_t generation,
                                                      std::uint64_t sourceVersion) {
  const auto rule = ByteRange(stylesheetRule.ruleSourceRange, source.size());
  const auto selector = ByteRange(stylesheetRule.selectorSourceRange, source.size());
  if (!rule || !selector || !Contains(*rule, offset) || source[rule->end - 1] != '}') {
    return std::nullopt;
  }
  StructuredCssTarget target{.rule = *rule,
                             .selector = *selector,
                             .insertDeclaration = rule->end - 1,
                             .selectionOffset = offset,
                             .generation = generation,
                             .sourceVersion = sourceVersion};
  if (stylesheetRule.stylesheetElement) {
    if (AppendDecoded(target.decodedMoveText, stylesheetRule.stylesheetElement->textContent(),
                      stylesheetRule.ruleCssRange)) {
      target.encoding = EmbeddedEncoding(*stylesheetRule.stylesheetElement, source, *rule);
    }
  }
  AddDeclarationTargets(stylesheetRule, source, offset, target);
  return target;
}

std::optional<StructuredCssTarget> FindInlineTarget(const svg::SVGDocument& document,
                                                    std::string_view source, std::size_t offset,
                                                    std::uint64_t generation) {
  const auto attribute = document.xmlDocument().attributeAtSourceOffset(offset);
  if (!attribute || attribute->name != xml::XMLQualifiedNameRef("style")) {
    return std::nullopt;
  }
  const auto valueRange = ByteRange(attribute->valueLocation, source.size());
  const auto value = attribute->node.getAttribute(attribute->name);
  if (!valueRange || !value) {
    return std::nullopt;
  }
  css::parser::DeclarationListParser::SecurityStats stats;
  const auto declarations =
      css::parser::DeclarationListParser::ParseOnlyDeclarations(*value, &stats);
  if (stats.rejected) {
    return std::nullopt;
  }
  StructuredCssTarget target{.rule = *valueRange,
                             .insertDeclaration = valueRange->end,
                             .encoding = StructuredCssSourceEncoding::Attribute,
                             .attributeQuote = attribute->quote,
                             .inlineStyle = true,
                             .selectionOffset = offset,
                             .generation = generation,
                             .sourceVersion = document.sourceVersion()};
  const auto map = [&](const std::optional<SourceRange>& range) -> std::optional<SourceByteRange> {
    return range ? ByteRange(attribute->node.mapDecodedAttributeRange(attribute->name, *range),
                             source.size())
                 : std::nullopt;
  };
  for (const auto& declaration : declarations) {
    AddDeclarationTarget(declaration, *value, source, offset, target, map);
  }
  return target;
}

struct CssReplacement {
  SourceByteRange range;
  std::string bytes;
};

std::optional<SourceByteRange> ReplacementExtent(const StructuredCssTarget& target,
                                                 StructuredCssOperation operation) {
  switch (operation) {
    case StructuredCssOperation::ReplaceSelector: return target.selector;
    case StructuredCssOperation::ReplaceName: return target.name;
    case StructuredCssOperation::ReplaceValue: return target.value;
    case StructuredCssOperation::RemoveDeclaration: return target.declaration;
    case StructuredCssOperation::RemoveRule: return target.rule;
    case StructuredCssOperation::RemoveImportant: return target.priority;
    default: return std::nullopt;
  }
}

bool AllowsEdit(const StructuredCssTarget& target, StructuredCssOperation operation) {
  using Operation = StructuredCssOperation;
  return !target.inlineStyle ||
         (operation != Operation::ReplaceSelector && operation != Operation::RemoveRule &&
          operation != Operation::InsertRule);
}

std::optional<CssReplacement> MakeReplacement(const StructuredCssTarget& target,
                                              StructuredCssOperation operation,
                                              std::string_view replacement) {
  using Operation = StructuredCssOperation;
  if (!AllowsEdit(target, operation)) {
    return std::nullopt;
  }
  switch (operation) {
    case Operation::AddImportant:
      if (target.priority || !target.value) {
        return std::nullopt;
      }
      return CssReplacement{{target.value->end, target.value->end}, " !important"};
    case Operation::InsertDeclaration: {
      std::string bytes = target.declarationNeedsSeparator ? "; " : " ";
      bytes.append(replacement);
      return CssReplacement{{target.insertDeclaration, target.insertDeclaration}, std::move(bytes)};
    }
    case Operation::InsertRule:
      return CssReplacement{{target.rule.end, target.rule.end}, "\n" + std::string(replacement)};
    default: break;
  }
  const auto range = ReplacementExtent(target, operation);
  return range ? std::optional(CssReplacement{*range, std::string(replacement)}) : std::nullopt;
}

bool IsCurrent(EditorApp& app, std::string_view source, const StructuredCssTarget& target) {
  return FindStructuredCssTarget(app, source, target.selectionOffset) == target;
}

bool ApplyReplacement(EditorApp& app, DocumentSyncController& sync, TextEditor& editor,
                      std::string_view source, const CssReplacement& edit,
                      std::uint64_t sourceVersion) {
  if (editor.hasPendingSourceEditIntents() || sync.hasPendingSourceTextSync() ||
      edit.range.start > edit.range.end || edit.range.end > source.size() ||
      source.substr(edit.range.start, edit.range.end - edit.range.start) == edit.bytes) {
    return false;
  }
  const auto& document = app.document().document();
  auto before = captureDocumentSourceSnapshot(document.svgElement(), document.source());
  const auto delta = CommitStructuredSourceEdit(app,
                                                {.offset = edit.range.start,
                                                 .removedLength = edit.range.end - edit.range.start,
                                                 .replacement = edit.bytes},
                                                sourceVersion);
  if (!delta) {
    return false;
  }
  const auto& committed = app.document().document();
  auto after = captureDocumentSourceSnapshot(committed.svgElement(), committed.source());
  app.undoTimeline().record("Edit CSS", std::move(before), std::move(after));
  std::ignore = sync.mirrorSourceDeltas(app, editor, {*delta});
  return true;
}

bool IsCssTrivia(std::string_view text) {
  while (!text.empty()) {
    if (const auto first = text.find_first_not_of(" \t\r\n\f"); first != std::string_view::npos) {
      text.remove_prefix(first);
    } else {
      return true;
    }
    if (!text.starts_with("/*")) {
      return false;
    }
    const auto end = text.find("*/", 2);
    if (end == std::string_view::npos) {
      return false;
    }
    text.remove_prefix(end + 2);
  }
  return true;
}

std::string_view Slice(std::string_view source, SourceByteRange range) {
  return source.substr(range.start, range.end - range.start);
}

bool Overlap(SourceByteRange first, SourceByteRange second) {
  return first.start < second.end && second.start < first.end;
}

bool EndsInSemicolon(std::string_view source, SourceByteRange range) {
  return Slice(source, range).ends_with(';');
}

bool IsCDataContent(std::string_view bytes) {
  if (bytes.find(']') == std::string_view::npos) {
    return true;
  }
  std::string wrapped = "<![CDATA[";
  wrapped += bytes;
  wrapped += "]]>";
  bool complete = false;
  xml::Tokenize(wrapped, [&](const xml::XMLToken& token) {
    complete |= token.type == xml::XMLTokenType::CData && token.range.start.offset == 0 &&
                token.range.end.offset == wrapped.size();
  });
  return complete;
}

bool SameEncoding(const StructuredCssTarget& from, const StructuredCssTarget& destination) {
  return from.encoding == destination.encoding && from.attributeQuote == destination.attributeQuote;
}

std::optional<RcString> EncodeMovedCss(const StructuredCssTarget& from,
                                       const StructuredCssTarget& destination,
                                       std::string_view original) {
  using Encoding = StructuredCssSourceEncoding;
  if (from.encoding == Encoding::Unmapped || destination.encoding == Encoding::Unmapped) {
    return std::nullopt;
  }
  if (SameEncoding(from, destination)) {
    return RcString(original);
  }
  switch (destination.encoding) {
    case Encoding::Attribute:
      return xml::EscapeAttributeValue(from.decodedMoveText, destination.attributeQuote);
    case Encoding::Text: return xml::EscapeTextContent(from.decodedMoveText);
    case Encoding::CData:
      return IsCDataContent(from.decodedMoveText) ? std::optional(RcString(from.decodedMoveText))
                                                  : std::nullopt;
    case Encoding::Unmapped: return std::nullopt;
  }
  return std::nullopt;
}

SourceByteRange Between(SourceByteRange first, SourceByteRange second) {
  return first.start < second.start ? SourceByteRange{first.end, second.start}
                                    : SourceByteRange{second.end, first.start};
}

CssReplacement SwapAdjacent(std::string_view source, SourceByteRange item, SourceByteRange anchor,
                            SourceByteRange gap) {
  const auto left = item.start < anchor.start ? item : anchor;
  const auto right = item.start < anchor.start ? anchor : item;
  std::string bytes(Slice(source, right));
  bytes += Slice(source, gap);
  bytes += Slice(source, left);
  return {{left.start, right.end}, std::move(bytes)};
}

CssReplacement CutAndInsert(std::string_view source, SourceByteRange from, std::size_t insertion,
                            std::string_view bytes) {
  if (insertion < from.start) {
    std::string replacement(bytes);
    replacement += source.substr(insertion, from.start - insertion);
    return {{insertion, from.end}, std::move(replacement)};
  }
  std::string replacement(source.substr(from.end, insertion - from.end));
  replacement += bytes;
  return {{from.start, insertion}, std::move(replacement)};
}

std::string TransferSeparator(std::string_view source, SourceByteRange destination) {
  const auto newline = source.rfind('\n', destination.start);
  if (newline == std::string_view::npos) {
    return " ";
  }
  const auto indentation = source.substr(newline + 1, destination.start - newline - 1);
  if (indentation.find_first_not_of(" \t") != std::string_view::npos) {
    return " ";
  }
  return "\n" + std::string(indentation);
}

std::string TransferText(std::string_view source, std::string_view moved, SourceByteRange to,
                         bool declaration, bool after) {
  std::string bytes;
  const auto separator = TransferSeparator(source, to);
  if (after) {
    bytes = declaration && !EndsInSemicolon(source, to) ? ";" + separator : separator;
  }
  bytes += moved;
  if (declaration && !moved.ends_with(';')) {
    bytes += ';';
  }
  if (!after) {
    bytes += separator;
  }
  return bytes;
}

std::optional<CssReplacement> PlanTransfer(std::string_view source, const StructuredCssTarget& from,
                                           const StructuredCssTarget& destination,
                                           SourceByteRange item, SourceByteRange anchor,
                                           std::size_t insertion, bool after) {
  const auto encoded = EncodeMovedCss(from, destination, Slice(source, item));
  if (!encoded) {
    return std::nullopt;
  }
  const auto gap = Between(item, anchor);
  const bool adjacent = IsCssTrivia(Slice(source, gap));
  if (adjacent && after == (item.start > anchor.start)) {
    return std::nullopt;
  }
  const bool declaration = from.declaration.has_value();
  if (adjacent && SameEncoding(from, destination) &&
      (!declaration || (EndsInSemicolon(source, item) && EndsInSemicolon(source, anchor)))) {
    return SwapAdjacent(source, item, anchor, gap);
  }
  const auto bytes = TransferText(source, *encoded, anchor, declaration, after);
  return CutAndInsert(source, item, insertion, bytes);
}

bool CompatibleMoveTargets(const StructuredCssTarget& from, const StructuredCssTarget& to) {
  return from.declaration.has_value() == to.declaration.has_value() &&
         (from.declaration || (!from.inlineStyle && !to.inlineStyle));
}

std::optional<CssReplacement> PlanMove(EditorApp& app, std::string_view source,
                                       const StructuredCssTarget& from,
                                       const StructuredCssTarget& destination, bool after) {
  if (!IsCurrent(app, source, from) || !IsCurrent(app, source, destination) ||
      !CompatibleMoveTargets(from, destination)) {
    return std::nullopt;
  }
  const auto item = from.declaration.value_or(from.rule);
  const auto anchor = destination.declaration.value_or(destination.rule);
  if (Overlap(item, anchor)) {
    return std::nullopt;
  }
  const auto insertion = after ? anchor.end : anchor.start;
  if (insertion == item.start || insertion == item.end) {
    return std::nullopt;
  }
  return PlanTransfer(source, from, destination, item, anchor, insertion, after);
}

}  // namespace

std::optional<StructuredCssTarget> FindStructuredCssTarget(EditorApp& app, std::string_view source,
                                                           std::size_t offset) {
  if (!app.hasDocument() || !app.structuredEditingEnabled() ||
      app.document().hasPendingMutations()) {
    return std::nullopt;
  }
  const auto& document = app.document().document();
  const auto access = document.tryReadAccess();
  if (!access || !MatchesDocumentSource(source, document.source())) {
    return std::nullopt;
  }
  const auto generation = app.document().documentGeneration();
  const auto stylesheetRule = svg::CollectStylesheetRuleAtSourceOffset(document, offset);
  if (!stylesheetRule) {
    return FindInlineTarget(document, source, offset, generation);
  }
  return MakeEmbeddedTarget(*stylesheetRule, source, offset, generation, document.sourceVersion());
}

bool ApplyStructuredCssEdit(EditorApp& app, DocumentSyncController& sync, TextEditor& editor,
                            const StructuredCssTarget& target, StructuredCssOperation operation,
                            std::string_view replacement) {
  const std::string source = editor.getText();
  if (!IsCurrent(app, source, target)) {
    return false;
  }
  const auto edit = MakeReplacement(target, operation, replacement);
  return edit && ApplyReplacement(app, sync, editor, source, *edit, target.sourceVersion);
}

bool CanMoveStructuredCss(EditorApp& app, std::string_view source, const StructuredCssTarget& from,
                          const StructuredCssTarget& destination, bool after) {
  return PlanMove(app, source, from, destination, after).has_value();
}

bool MoveStructuredCss(EditorApp& app, DocumentSyncController& sync, TextEditor& editor,
                       const StructuredCssTarget& from, const StructuredCssTarget& destination,
                       bool after) {
  const std::string source = editor.getText();
  const auto edit = PlanMove(app, source, from, destination, after);
  return edit && ApplyReplacement(app, sync, editor, source, *edit, from.sourceVersion);
}

}  // namespace donner::editor
