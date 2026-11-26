#include "donner/editor/StructuredCssEdit.h"

#include <algorithm>

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

void AddDeclarationTargets(const svg::SVGStylesheetRule& rule, std::string_view source,
                           std::size_t offset, StructuredCssTarget& target) {
  const auto map = [&](const std::optional<SourceRange>& range) -> std::optional<SourceByteRange> {
    return range && rule.stylesheetElement
               ? ByteRange(svg::MapStylesheetSourceRange(*rule.stylesheetElement, *range),
                           source.size())
               : std::nullopt;
  };
  for (const auto& entry : rule.declarations) {
    const auto& declaration = entry.declaration;
    const auto extent = map(declaration.authoredDeclarationRange());
    if (!extent) {
      continue;
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
}

struct CssReplacement {
  SourceByteRange range;
  std::string bytes;
};

std::optional<CssReplacement> MakeReplacement(const StructuredCssTarget& target,
                                              StructuredCssOperation operation,
                                              std::string_view replacement) {
  const auto replace = [&](const std::optional<SourceByteRange>& range,
                           std::string_view bytes) -> std::optional<CssReplacement> {
    return range ? std::optional(CssReplacement{*range, std::string(bytes)}) : std::nullopt;
  };
  switch (operation) {
    case StructuredCssOperation::ReplaceSelector: return replace(target.selector, replacement);
    case StructuredCssOperation::ReplaceName: return replace(target.name, replacement);
    case StructuredCssOperation::ReplaceValue: return replace(target.value, replacement);
    case StructuredCssOperation::RemoveDeclaration: return replace(target.declaration, {});
    case StructuredCssOperation::RemoveRule: return replace(target.rule, {});
    case StructuredCssOperation::RemoveImportant: return replace(target.priority, {});
    case StructuredCssOperation::AddImportant:
      if (target.priority || !target.value) {
        return std::nullopt;
      }
      return CssReplacement{{target.value->end, target.value->end}, " !important"};
    case StructuredCssOperation::InsertDeclaration: {
      std::string bytes = target.declarationNeedsSeparator ? "; " : " ";
      bytes.append(replacement);
      return CssReplacement{{target.insertDeclaration, target.insertDeclaration}, std::move(bytes)};
    }
    case StructuredCssOperation::InsertRule:
      return CssReplacement{{target.rule.end, target.rule.end}, "\n" + std::string(replacement)};
  }
  return std::nullopt;
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

bool IsMovableDeclarationPair(const StructuredCssTarget& from, const StructuredCssTarget& before,
                              std::string_view source) {
  return from.rule == before.rule && source[from.declaration->end - 1] == ';' &&
         source[before.declaration->end - 1] == ';';
}

std::optional<CssReplacement> MakeMove(std::string_view source, SourceByteRange from,
                                       SourceByteRange before) {
  if (from.start == before.start) {
    return std::nullopt;
  }
  const bool movingEarlier = from.start > before.start;
  const auto left = movingEarlier ? before : from;
  const auto right = movingEarlier ? from : before;
  if (left.end > right.start) {
    return std::nullopt;
  }
  const auto gap = source.substr(left.end, right.start - left.end);
  if (!IsCssTrivia(gap) || !movingEarlier) {
    return std::nullopt;
  }
  std::string bytes(source.substr(right.start, right.end - right.start));
  bytes += gap;
  bytes += source.substr(left.start, left.end - left.start);
  return CssReplacement{{left.start, right.end}, std::move(bytes)};
}

}  // namespace

std::optional<StructuredCssTarget> FindStructuredCssTarget(EditorApp& app, std::string_view source,
                                                           std::size_t offset) {
  if (!app.hasDocument() || !app.structuredEditingEnabled()) {
    return std::nullopt;
  }
  const auto& document = app.document().document();
  const auto access = document.tryReadAccess();
  if (!access || !MatchesDocumentSource(source, document.source())) {
    return std::nullopt;
  }
  for (const auto& stylesheetRule : svg::CollectStylesheetRules(document)) {
    const auto rule = ByteRange(stylesheetRule.ruleSourceRange, source.size());
    const auto selector = ByteRange(stylesheetRule.selectorSourceRange, source.size());
    if (!rule || !selector || !Contains(*rule, offset) || rule->end <= rule->start ||
        source[rule->end - 1] != '}') {
      continue;
    }
    StructuredCssTarget target{.rule = *rule,
                               .selector = *selector,
                               .insertDeclaration = rule->end - 1,
                               .selectionOffset = offset,
                               .generation = app.document().documentGeneration(),
                               .sourceVersion = document.sourceVersion()};
    AddDeclarationTargets(stylesheetRule, source, offset, target);
    return target;
  }
  return std::nullopt;
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

bool MoveStructuredCssBefore(EditorApp& app, TextEditor& editor, const StructuredCssTarget& from,
                             const StructuredCssTarget& before) {
  const std::string source = editor.getText();
  if (!IsCurrent(app, source, from) || !IsCurrent(app, source, before) ||
      from.declaration.has_value() != before.declaration.has_value()) {
    return false;
  }
  if (from.declaration && !IsMovableDeclarationPair(from, before, source)) {
    return false;
  }
  const auto edit = MakeMove(source, from.declaration.value_or(from.rule),
                             before.declaration.value_or(before.rule));
  return edit && ApplyReplacement(editor, source, *edit);
}

}  // namespace donner::editor
