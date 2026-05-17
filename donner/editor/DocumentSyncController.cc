#include "donner/editor/DocumentSyncController.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <string_view>

#include "donner/editor/SelectTool.h"
#include "donner/editor/SourceSync.h"
#include "donner/editor/TextPatch.h"

namespace donner::editor {

namespace {

constexpr float kTextChangeDebounceSeconds = 0.15f;
constexpr int kNoErrorLine = -1;

struct SourceMirrorEdit {
  std::size_t offset = 0;
  std::size_t removedLength = 0;
  std::string_view replacement;
};

std::optional<SourceMirrorEdit> BuildSingleSourceMirrorEdit(std::string_view oldSource,
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

  return SourceMirrorEdit{
      .offset = prefixLength,
      .removedLength = oldSource.size() - prefixLength - suffixLength,
      .replacement = newSource.substr(prefixLength, newSource.size() - prefixLength - suffixLength),
  };
}

std::string CanonicalizeForTextEditor(std::string_view source) {
  std::string result(source);
  if (!result.empty() && result.back() == '\n') {
    result.pop_back();
  }
  return result;
}

bool MirrorDocumentSourceIntoTextEditor(EditorApp& app, TextEditor& textEditor,
                                        std::string* previousSourceText,
                                        std::optional<std::string>* lastWritebackSourceText) {
  if (!app.hasDocument() || !app.document().document().hasSourceStore()) {
    return false;
  }

  std::string source = CanonicalizeForTextEditor(app.document().document().source());
  const std::string currentText = textEditor.getText();
  if (currentText != source) {
    if (std::optional<SourceMirrorEdit> edit = BuildSingleSourceMirrorEdit(currentText, source)) {
      textEditor.applyExternalSourceEdit(edit->offset, edit->removedLength, edit->replacement);
    }

    if (textEditor.getText() != source) {
      textEditor.setText(source, /*preserveScroll=*/true);
      textEditor.resetTextChanged();
    }
  }
  *previousSourceText = source;
  *lastWritebackSourceText = source;
  app.syncDirtyFromSource(source);
  return true;
}

}  // namespace

DocumentSyncController::DocumentSyncController(std::string initialSource)
    : previousSourceText_(std::move(initialSource)) {}

void DocumentSyncController::resetForLoadedDocument(const std::string& source) {
  previousSourceText_ = source;
  lastWritebackSourceText_.reset();
  pendingTransformWritebacks_.clear();
  pendingElementRemoveWritebacks_.clear();
  pendingSourceEditIntents_.clear();
  lastShownErrorLine_ = kNoErrorLine;
  lastShownErrorReason_.clear();
  textChangePending_ = false;
  textDispatchThrottled_ = false;
  textChangeIdleTimer_ = 0.0f;
}

void DocumentSyncController::syncParseErrorMarkers(EditorApp& app, TextEditor& textEditor) {
  const auto& parseError = app.document().lastParseError();
  if (parseError.has_value()) {
    const int line = parseError->range.start.lineInfo.has_value()
                         ? static_cast<int>(parseError->range.start.lineInfo->line)
                         : 1;
    const std::string_view reasonSv = parseError->reason;
    if (line != lastShownErrorLine_ || reasonSv != lastShownErrorReason_) {
      textEditor.setErrorMarkers(ParseErrorToMarkers(*parseError));
      lastShownErrorLine_ = line;
      lastShownErrorReason_.assign(reasonSv);
    }
  } else if (lastShownErrorLine_ != kNoErrorLine) {
    textEditor.setErrorMarkers({});
    lastShownErrorLine_ = kNoErrorLine;
    lastShownErrorReason_.clear();
  }
}

void DocumentSyncController::handleTextEdits(EditorApp& app, TextEditor& textEditor,
                                             float deltaSeconds) {
  const auto dispatchTextChange = [&](std::string_view newSource) {
    if (pendingSourceEditIntents_.empty()) {
      (void)DispatchSourceTextChange(app, newSource, &previousSourceText_,
                                     &lastWritebackSourceText_);
      return;
    }

    (void)DispatchSourceEditIntents(app, pendingSourceEditIntents_, newSource, &previousSourceText_,
                                    &lastWritebackSourceText_);
    pendingSourceEditIntents_.clear();
  };

  if (textEditor.isTextChanged()) {
    const std::string newSource = textEditor.getText();
    std::vector<SourceEditIntent> editIntents = textEditor.takePendingSourceEditIntents();
    pendingSourceEditIntents_.insert(pendingSourceEditIntents_.end(),
                                     std::make_move_iterator(editIntents.begin()),
                                     std::make_move_iterator(editIntents.end()));
    app.syncDirtyFromSource(newSource);
    textEditor.resetTextChanged();

    if (!textDispatchThrottled_) {
      dispatchTextChange(newSource);
      textDispatchThrottled_ = true;
      textChangePending_ = false;
    } else {
      textChangePending_ = true;
    }
    textChangeIdleTimer_ = 0.0f;
  } else if (textDispatchThrottled_) {
    textChangeIdleTimer_ += deltaSeconds;
    if (textChangeIdleTimer_ >= kTextChangeDebounceSeconds) {
      if (textChangePending_) {
        const std::string newSource = textEditor.getText();
        app.syncDirtyFromSource(newSource);
        dispatchTextChange(newSource);
        textChangePending_ = false;
      }
      textDispatchThrottled_ = false;
      textChangeIdleTimer_ = 0.0f;
    }
  }
}

void DocumentSyncController::applyPendingWritebacks(EditorApp& app, SelectTool& selectTool,
                                                    TextEditor& textEditor) {
  if (auto completed = selectTool.consumeCompletedDragWriteback(); completed.has_value()) {
    pendingTransformWritebacks_.push_back(EditorApp::CompletedTransformWriteback{
        .target = std::move(completed->target),
        .transform = completed->transform,
    });
    for (auto& extra : completed->extras) {
      pendingTransformWritebacks_.push_back(EditorApp::CompletedTransformWriteback{
          .target = std::move(extra.target),
          .transform = extra.transform,
      });
    }
  }
  if (auto completed = app.consumeTransformWriteback(); completed.has_value()) {
    pendingTransformWritebacks_.push_back(std::move(*completed));
  }

  auto completedRemovals = app.consumeElementRemoveWritebacks();
  for (auto& writeback : completedRemovals) {
    pendingElementRemoveWritebacks_.push_back(std::move(writeback));
  }

  if (!pendingElementRemoveWritebacks_.empty()) {
    if (MirrorDocumentSourceIntoTextEditor(app, textEditor, &previousSourceText_,
                                           &lastWritebackSourceText_)) {
      pendingElementRemoveWritebacks_.clear();
    } else {
      std::string source = textEditor.getText();
      bool changed = false;
      for (const auto& pendingRemove : pendingElementRemoveWritebacks_) {
        auto patch = buildElementRemoveWriteback(source, pendingRemove.target);
        if (!patch.has_value()) {
          continue;
        }

        applyPatches(source, {{*patch}});
        changed = true;
      }

      pendingElementRemoveWritebacks_.clear();
      if (changed) {
        textEditor.setText(source, /*preserveScroll=*/true);
        QueueSourceWritebackReparse(app, source, &previousSourceText_, &lastWritebackSourceText_);
      }
    }
  }

  if (pendingTransformWritebacks_.empty()) {
    return;
  }

  if (app.hasDocument() && app.document().document().hasSourceStore()) {
    svg::SVGDocument& document = app.document().document();
    for (const auto& writeback : pendingTransformWritebacks_) {
      std::optional<svg::SVGElement> element =
          resolveAttributeWritebackTarget(document, writeback.target);
      if (!element.has_value()) {
        continue;
      }

      if (writeback.restoreSourceTransformAttributeValue) {
        if (writeback.sourceTransformAttributeValue.has_value()) {
          element->setAttribute("transform", *writeback.sourceTransformAttributeValue);
        } else {
          element->removeAttribute("transform");
        }
      } else {
        const RcString serialized = toSVGTransformString(writeback.transform);
        if (std::string_view(serialized).empty()) {
          element->removeAttribute("transform");
        } else {
          element->setAttribute("transform", serialized);
        }
      }
    }

    pendingTransformWritebacks_.clear();
    (void)MirrorDocumentSourceIntoTextEditor(app, textEditor, &previousSourceText_,
                                             &lastWritebackSourceText_);
    return;
  }

  std::string source = textEditor.getText();
  std::vector<TextPatch> patches;
  patches.reserve(pendingTransformWritebacks_.size());
  for (const auto& writeback : pendingTransformWritebacks_) {
    std::optional<TextPatch> patch;
    if (writeback.restoreSourceTransformAttributeValue) {
      if (writeback.sourceTransformAttributeValue.has_value()) {
        patch = buildAttributeWriteback(source, writeback.target, "transform",
                                        std::string_view(*writeback.sourceTransformAttributeValue));
      } else {
        patch = buildAttributeRemoveWriteback(source, writeback.target, "transform");
      }
    } else {
      const RcString serialized = toSVGTransformString(writeback.transform);
      if (std::string_view(serialized).empty()) {
        patch = buildAttributeRemoveWriteback(source, writeback.target, "transform");
      } else {
        patch = buildAttributeWriteback(source, writeback.target, "transform",
                                        std::string_view(serialized));
      }
    }
    if (patch.has_value()) {
      patches.push_back(*std::move(patch));
    }
  }
  pendingTransformWritebacks_.clear();
  if (patches.empty()) {
    return;
  }

  applyPatches(source, patches);
  textEditor.setText(source, /*preserveScroll=*/true);
  QueueSourceWritebackReparse(app, source, &previousSourceText_, &lastWritebackSourceText_);
}

TextEditor::ErrorMarkers DocumentSyncController::ParseErrorToMarkers(const ParseDiagnostic& diag) {
  TextEditor::ErrorMarkers markers;
  const auto& start = diag.range.start;
  const int line = start.lineInfo.has_value() ? static_cast<int>(start.lineInfo->line) : 1;
  markers.emplace(line, std::string(std::string_view(diag.reason)));
  return markers;
}

}  // namespace donner::editor
