#include "donner/editor/SourceSync.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "donner/base/FileOffset.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorCommand.h"

namespace donner::editor {

namespace {

struct SourceTextEdit {
  std::size_t offset = 0;
  std::size_t removedLength = 0;
  std::string_view replacement;
};

struct StructuredApplyResult {
  bool applied = false;
  bool needsDocumentReplace = false;
};

std::vector<AttributeWritebackTarget> CaptureSelectionTargets(const EditorApp& app) {
  std::vector<AttributeWritebackTarget> targets;
  targets.reserve(app.selectedElements().size());
  for (const svg::SVGElement& element : app.selectedElements()) {
    if (std::optional<AttributeWritebackTarget> target = captureAttributeWritebackTarget(element);
        target.has_value()) {
      targets.push_back(std::move(*target));
    }
  }

  return targets;
}

void RemapSelectionAfterStructuredSourceEdit(EditorApp& app,
                                             const std::vector<AttributeWritebackTarget>& targets) {
  if (targets.empty() && app.selectedElements().empty()) {
    return;
  }

  std::vector<svg::SVGElement> remappedSelection;
  remappedSelection.reserve(targets.size());
  for (const AttributeWritebackTarget& target : targets) {
    if (std::optional<svg::SVGElement> element =
            resolveAttributeWritebackTarget(app.document().document(), target);
        element.has_value()) {
      remappedSelection.push_back(*element);
    }
  }

  app.setSelection(std::move(remappedSelection));
}

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

std::optional<std::string> ApplySourceTextEdit(std::string_view source,
                                               const SourceEditIntent& intent) {
  if (intent.offset > source.size() || intent.removedLength > source.size() - intent.offset) {
    return std::nullopt;
  }

  std::string result(source);
  result.replace(intent.offset, intent.removedLength, intent.replacement);
  return result;
}

StructuredApplyResult TryApplyStructuredSourceEdit(EditorApp& app, std::string_view previousSource,
                                                   const SourceTextEdit& edit) {
  if (!app.structuredEditingEnabled() || !app.hasDocument()) {
    return {};
  }

  const std::vector<AttributeWritebackTarget> selectionTargets = CaptureSelectionTargets(app);

  svg::SVGDocument& document = app.document().document();
  if (!document.hasSourceStore() || document.source() != previousSource) {
    return {};
  }

  xml::ApplySourceEditResult result = app.document().applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(edit.offset),
                           FileOffset::Offset(edit.offset + edit.removedLength)},
      .replacement = edit.replacement,
      .sourceVersion = document.sourceVersion(),
  });

  if (!result.applied) {
    return {};
  }

  if (result.diagnostic.has_value()) {
    app.setSelection(std::nullopt);
  } else {
    RemapSelectionAfterStructuredSourceEdit(app, selectionTargets);
  }

  return StructuredApplyResult{
      .applied = true,
      .needsDocumentReplace = result.scope == xml::ReparseScope::Document,
  };
}

bool TryApplyStructuredSourceChange(EditorApp& app, std::string_view previousSource,
                                    std::string_view newSource) {
  std::optional<SourceTextEdit> edit = BuildSingleSourceTextEdit(previousSource, newSource);
  if (!edit.has_value()) {
    return false;
  }

  const StructuredApplyResult result = TryApplyStructuredSourceEdit(app, previousSource, *edit);
  if (!result.applied) {
    return false;
  }

  if (result.needsDocumentReplace || app.document().document().source() != newSource) {
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
  handled = TryApplyStructuredSourceChange(app, *previousSourceText, newSource);

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

DispatchSourceTextChangeResult DispatchSourceEditIntents(
    EditorApp& app, const std::vector<SourceEditIntent>& intents, std::string_view newSource,
    std::string* previousSourceText, std::optional<std::string>* lastWritebackSourceText) {
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

  if (intents.empty()) {
    return DispatchSourceTextChange(app, newSource, previousSourceText, lastWritebackSourceText);
  }

  std::string workingSource = *previousSourceText;
  for (const SourceEditIntent& intent : intents) {
    std::optional<std::string> nextSource = ApplySourceTextEdit(workingSource, intent);
    if (!nextSource.has_value()) {
      app.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(newSource)));
      *previousSourceText = std::string(newSource);
      lastWritebackSourceText->reset();
      return DispatchSourceTextChangeResult{
          .dispatchedMutation = true,
          .skippedSelfWriteback = false,
      };
    }

    const SourceTextEdit edit{
        .offset = intent.offset,
        .removedLength = intent.removedLength,
        .replacement = intent.replacement,
    };
    const StructuredApplyResult result = TryApplyStructuredSourceEdit(app, workingSource, edit);
    if (!result.applied || result.needsDocumentReplace) {
      app.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(newSource)));
      *previousSourceText = std::string(newSource);
      lastWritebackSourceText->reset();
      return DispatchSourceTextChangeResult{
          .dispatchedMutation = true,
          .skippedSelfWriteback = false,
      };
    }

    if (app.document().document().source() != *nextSource) {
      app.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(newSource)));
      *previousSourceText = std::string(newSource);
      lastWritebackSourceText->reset();
      return DispatchSourceTextChangeResult{
          .dispatchedMutation = true,
          .skippedSelfWriteback = false,
      };
    }

    workingSource = std::move(*nextSource);
  }

  if (workingSource != newSource) {
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
