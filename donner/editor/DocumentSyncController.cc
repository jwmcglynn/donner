#include "donner/editor/DocumentSyncController.h"

#include "donner/editor/SelectTool.h"
#include "donner/editor/SourceSync.h"
#include "donner/editor/TextPatch.h"

namespace donner::editor {

namespace {

constexpr float kTextChangeDebounceSeconds = 0.15f;
constexpr int kNoErrorLine = -1;

}  // namespace

DocumentSyncController::DocumentSyncController(std::string initialSource)
    : previousSourceText_(std::move(initialSource)) {}

void DocumentSyncController::resetForLoadedDocument(const std::string& source) {
  previousSourceText_ = source;
  lastWritebackSourceText_.reset();
  pendingTransformWriteback_.reset();
  pendingElementRemoveWritebacks_.clear();
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
  const auto dispatchTextChange = [&]() {
    const std::string newSource = textEditor.getText();
    (void)DispatchSourceTextChange(app, newSource, &previousSourceText_, &lastWritebackSourceText_);
  };

  if (textEditor.isTextChanged()) {
    app.markDirty();
    textEditor.resetTextChanged();

    if (!textDispatchThrottled_) {
      dispatchTextChange();
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
        dispatchTextChange();
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
    pendingTransformWriteback_ = EditorApp::CompletedTransformWriteback{
        .target = std::move(completed->target),
        .transform = completed->transform,
    };
  }
  if (auto completed = app.consumeTransformWriteback(); completed.has_value()) {
    pendingTransformWriteback_ = std::move(*completed);
  }

  auto completedRemovals = app.consumeElementRemoveWritebacks();
  for (auto& writeback : completedRemovals) {
    pendingElementRemoveWritebacks_.push_back(std::move(writeback));
  }

  if (!pendingElementRemoveWritebacks_.empty()) {
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

  if (!pendingTransformWriteback_.has_value()) {
    return;
  }

  std::string source = textEditor.getText();
  const RcString serialized = toSVGTransformString(pendingTransformWriteback_->transform);
  if (std::string_view(serialized).empty()) {
    auto patch =
        buildAttributeRemoveWriteback(source, pendingTransformWriteback_->target, "transform");
    pendingTransformWriteback_.reset();
    if (!patch.has_value()) {
      return;
    }

    applyPatches(source, {{*patch}});
    textEditor.setText(source, /*preserveScroll=*/true);
    QueueSourceWritebackReparse(app, source, &previousSourceText_, &lastWritebackSourceText_);
    return;
  }

  auto patch = buildAttributeWriteback(source, pendingTransformWriteback_->target, "transform",
                                       std::string_view(serialized));
  if (!patch.has_value()) {
    return;
  }

  applyPatches(source, {{*patch}});
  textEditor.setText(source, /*preserveScroll=*/true);
  QueueSourceWritebackReparse(app, source, &previousSourceText_, &lastWritebackSourceText_);
  pendingTransformWriteback_.reset();
}

TextEditor::ErrorMarkers DocumentSyncController::ParseErrorToMarkers(const ParseDiagnostic& diag) {
  TextEditor::ErrorMarkers markers;
  const auto& start = diag.range.start;
  const int line = start.lineInfo.has_value() ? static_cast<int>(start.lineInfo->line) : 1;
  markers.emplace(line, std::string(std::string_view(diag.reason)));
  return markers;
}

}  // namespace donner::editor
