#include "donner/editor/SourceSync.h"

#include <algorithm>
#include <optional>

#include "donner/base/FileOffset.h"
#include "donner/editor/EditorCommand.h"

namespace donner::editor {

namespace {

struct SourceTextEdit {
  std::size_t offset = 0;
  std::size_t removedLength = 0;
  std::string_view replacement;
};

std::optional<SourceTextEdit> BuildSingleSourceTextEdit(std::string_view oldSource,
                                                        std::string_view newSource) {
  if (oldSource == newSource) {
    return std::nullopt;
  }

  std::size_t prefixLength = 0;
  const std::size_t commonLimit = std::min(oldSource.size(), newSource.size());
  while (prefixLength < commonLimit && oldSource[prefixLength] == newSource[prefixLength]) {
    ++prefixLength;
  }

  std::size_t suffixLength = 0;
  while (suffixLength < oldSource.size() - prefixLength &&
         suffixLength < newSource.size() - prefixLength &&
         oldSource[oldSource.size() - suffixLength - 1] ==
             newSource[newSource.size() - suffixLength - 1]) {
    ++suffixLength;
  }

  return SourceTextEdit{
      .offset = prefixLength,
      .removedLength = oldSource.size() - prefixLength - suffixLength,
      .replacement = newSource.substr(prefixLength, newSource.size() - prefixLength - suffixLength),
  };
}

bool TryApplyStructuredSourceEdit(EditorApp& app, std::string_view previousSource,
                                  std::string_view newSource) {
  if (!app.structuredEditingEnabled() || !app.hasDocument()) {
    return false;
  }

  svg::SVGDocument& document = app.document().document();
  if (!document.hasSourceStore() || document.source() != previousSource) {
    return false;
  }

  std::optional<SourceTextEdit> edit = BuildSingleSourceTextEdit(previousSource, newSource);
  if (!edit.has_value()) {
    return false;
  }

  xml::ApplySourceEditResult result = app.document().applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(edit->offset),
                           FileOffset::Offset(edit->offset + edit->removedLength)},
      .replacement = edit->replacement,
      .sourceVersion = document.sourceVersion(),
  });

  if (!result.applied) {
    return false;
  }

  if (result.scope == xml::ReparseScope::Document) {
    app.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(newSource)));
  }

  return true;
}

}  // namespace

void QueueSourceWritebackReparse(EditorApp& app, std::string_view newSource,
                                 std::string* previousSourceText,
                                 std::optional<std::string>* lastWritebackSourceText) {
  *previousSourceText = std::string(newSource);
  *lastWritebackSourceText = *previousSourceText;

  app.applyMutation(
      EditorCommand::ReplaceDocumentCommand(*previousSourceText, /*preserveUndoOnReparse=*/true));
}

DispatchSourceTextChangeResult DispatchSourceTextChange(
    EditorApp& app, std::string_view newSource, std::string* previousSourceText,
    std::optional<std::string>* lastWritebackSourceText) {
  if (lastWritebackSourceText->has_value() && newSource == **lastWritebackSourceText) {
    *previousSourceText = std::string(newSource);
    lastWritebackSourceText->reset();
    return DispatchSourceTextChangeResult{
        .dispatchedMutation = false,
        .skippedSelfWriteback = true,
    };
  }

  if (newSource == *previousSourceText) {
    return {};
  }

  bool handled = false;
  handled = TryApplyStructuredSourceEdit(app, *previousSourceText, newSource);

  if (!handled) {
    app.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(newSource)));
  }

  *previousSourceText = std::string(newSource);
  lastWritebackSourceText->reset();
  return DispatchSourceTextChangeResult{
      .dispatchedMutation = true,
      .skippedSelfWriteback = false,
  };
}

}  // namespace donner::editor
