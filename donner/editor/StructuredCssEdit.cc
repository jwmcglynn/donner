#include "donner/editor/StructuredCssEdit.h"

#include <algorithm>

#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLTokenizer.h"
#include "donner/css/parser/DeclarationListParser.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/TextEditor.h"
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

template <typename Mapper>
void AddDeclarationTarget(const css::Declaration& declaration, std::string_view source,
                          std::size_t offset, StructuredCssTarget& target, Mapper map) {
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
  for (const auto& entry : rule.declarations) {
    AddDeclarationTarget(entry.declaration, source, offset, target, map);
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
    AddDeclarationTarget(declaration, source, offset, target, map);
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

bool ApplyReplacement(TextEditor& editor, std::string_view source, const CssReplacement& edit) {
  if (edit.range.start > edit.range.end || edit.range.end > source.size() ||
      source.substr(edit.range.start, edit.range.end - edit.range.start) == edit.bytes) {
    return false;
  }
  editor.selectAndFocus(editor.getCoordinatesAtByteOffset(edit.range.start),
                        editor.getCoordinatesAtByteOffset(edit.range.end));
  editor.insertText(edit.bytes);
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

bool CanTransferLiteral(std::string_view bytes, const StructuredCssTarget& destination) {
  // Moving independently projected spans must not reinterpret XML entities or delimiters.
  return bytes.find_first_of("<&") == std::string_view::npos && IsCDataContent(bytes) &&
         (!destination.attributeQuote ||
          bytes.find(destination.attributeQuote) == std::string_view::npos);
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

std::string TransferText(std::string_view source, SourceByteRange from, SourceByteRange to,
                         bool declaration, bool after) {
  std::string bytes;
  const auto separator = TransferSeparator(source, to);
  if (after) {
    bytes = declaration && !EndsInSemicolon(source, to) ? ";" + separator : separator;
  }
  bytes += Slice(source, from);
  if (declaration && !EndsInSemicolon(source, from)) {
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
  if (!CanTransferLiteral(Slice(source, item), destination)) {
    return std::nullopt;
  }
  const auto gap = Between(item, anchor);
  const bool adjacent = IsCssTrivia(Slice(source, gap));
  if (adjacent && after == (item.start > anchor.start)) {
    return std::nullopt;
  }
  const bool declaration = from.declaration.has_value();
  if (adjacent &&
      (!declaration || (EndsInSemicolon(source, item) && EndsInSemicolon(source, anchor)))) {
    return SwapAdjacent(source, item, anchor, gap);
  }
  const auto bytes = TransferText(source, item, anchor, declaration, after);
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

bool ApplyStructuredCssEdit(EditorApp& app, TextEditor& editor, const StructuredCssTarget& target,
                            StructuredCssOperation operation, std::string_view replacement) {
  const std::string source = editor.getText();
  if (!IsCurrent(app, source, target)) {
    return false;
  }
  const auto edit = MakeReplacement(target, operation, replacement);
  return edit && ApplyReplacement(editor, source, *edit);
}

bool CanMoveStructuredCss(EditorApp& app, std::string_view source, const StructuredCssTarget& from,
                          const StructuredCssTarget& destination, bool after) {
  return PlanMove(app, source, from, destination, after).has_value();
}

bool MoveStructuredCss(EditorApp& app, TextEditor& editor, const StructuredCssTarget& from,
                       const StructuredCssTarget& destination, bool after) {
  const std::string source = editor.getText();
  const auto edit = PlanMove(app, source, from, destination, after);
  return edit && ApplyReplacement(editor, source, *edit);
}

}  // namespace donner::editor
