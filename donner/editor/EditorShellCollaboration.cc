/// @file
/// Native collaboration shares the editor's frame, source synchronization and undo path.
#include <algorithm>
#include <cstdio>

#include "donner/editor/EditorCollaboration.h"
#include "donner/editor/EditorShell.h"
#include "donner/editor/EditorShellInternal.h"
#include "donner/editor/LocalEditorControl.h"
#include "donner/editor/gui/EditorWindow.h"

namespace donner::editor {
bool EditorShell::initializeCollaboration() {
  if (!options_.controlSocketPath) return true;
  collaboration_ = std::make_unique<EditorCollaboration>(
      app_, EditorCollaboration::Callbacks{
                .flush =
                    [this]() {
                      const bool flushed = flushQueuedMutationAndRefreshOverlay();
                      renderCoordinator_.refreshSelectionBoundsCache(app_);
                      requestRenderAtEndOfFrame_ = true;
                      window_.wakeEventLoop();
                      return flushed;
                    },
                .save =
                    [this](std::string* error) {
                      if (!options_.allowFileSystemActions || !app_.currentFilePath()) {
                        *error =
                            "Save the document to a file in the editor before using remote save";
                        return false;
                      }
                      return trySavePath(*app_.currentFilePath(), error);
                    }});
  editorControl_ = std::make_unique<LocalEditorControl>();
  std::string error;
  if (!editorControl_->start(
          *options_.controlSocketPath, [this]() { window_.wakeEventLoop(); }, &error)) {
    std::fprintf(stderr, "Collaboration: %s\n", error.c_str());
    return false;
  }
  if (auto feedback = editorControl_->readFeedback()) collaboration_->restoreFeedback(*feedback);
  std::fprintf(stderr, "Native editor collaboration listening at %s\n",
               options_.controlSocketPath->c_str());
  return true;
}

bool EditorShell::collaborationFrameReady() {
  if (selectTool_.isDragging() || penTool_.isDrafting() ||
      renderCoordinator_.asyncRenderer().isBusy() ||
      documentSyncController_.hasPendingWritebacks() || textEditor_.isTextChanged())
    return false;
  if (app_.hasDocument()) {
    auto& document = app_.document().document();
    auto access = document.readAccess();
    if (textEditor_.getText() != internal::CanonicalizeForTextEditor(document.source()))
      return false;
  }
  return true;
}

void EditorShell::processCollaboration() {
  if (!editorControl_ || !editorControl_->hasPending()) return;
  editorControl_->process([this](const nlohmann::json& request) -> std::optional<nlohmann::json> {
    if (EditorCollaboration::requiresIdleDocument(request)) {
      if (!collaborationFrameReady()) return std::nullopt;
      collaboration_->refreshCommentAnchors();
    }
    if (collaboration_->shouldWaitForFeedback(request)) return std::nullopt;
    return collaboration_->handleRequest(request);
  });
  persistCollaborationFeedback();
}

void EditorShell::persistCollaborationFeedback() {
  if (!collaboration_ || !collaboration_->feedbackDirty() || !options_.controlSocketPath) return;
  std::string error;
  if (editorControl_->writeFeedback(collaboration_->feedbackArchive(), &error)) {
    collaboration_->markFeedbackClean();
    commentsPresenter_.setPersistenceError("");
  } else
    commentsPresenter_.setPersistenceError(std::move(error));
}

void EditorShell::renderCollaborationPanel() {
  if (!collaboration_) return;
  const auto previousRevision = collaboration_->feedbackRevision();
  const auto windowSize = window_.windowSize();
  const Box2d bounds =
      Box2d::FromXYWH(std::max(8.0f, static_cast<float>(windowSize.x) - rightPaneWidth_ + 8.0f),
                      42.0, std::max(200.0f, rightPaneWidth_ - 16.0f),
                      std::clamp(static_cast<double>(windowSize.y) - 52.0, 200.0, 430.0));
  commentsPresenter_.drawPanel(*collaboration_, !renderCoordinator_.asyncRenderer().isBusy(),
                               bounds);
  if (collaboration_->feedbackRevision() != previousRevision) window_.wakeEventLoop();
  persistCollaborationFeedback();
}
bool EditorShell::collaborationCanvasControlHovered(Vector2d point) const {
  return internal::CanvasScrollbarsCaptureInput(adaptiveUiLayout_.showCanvasScrollbars,
                                                interactionController_.viewport(), point) ||
         commentsPresenter_.capturesInput(point);
}

void EditorShell::renderCollaborationPins(const ViewportState& viewport, bool liveDrag) {
  if (!collaboration_) return;
  if (!renderCoordinator_.asyncRenderer().isBusy() &&
      (liveDrag || app_.document().currentFrameVersion() <=
                       renderCoordinator_.displayedDocVersionForDiagnostics())) {
    collaboration_->refreshCommentAnchors();
    commentsPresenter_.refreshPendingAnchor(*collaboration_);
  }
  commentsPresenter_.drawPins(*collaboration_, viewport);
}

void EditorShell::renderCollaborationContextMenu(bool rendererBusy) {
  if (!collaboration_ || !renderContextMenuDocumentPoint_) return;
  ImGui::Separator();
  if (ImGui::MenuItem("Add Comment Here", nullptr, false, app_.hasDocument() && !rendererBusy)) {
    commentsPresenter_.beginComment(*collaboration_, *renderContextMenuDocumentPoint_,
                                    renderContextMenuHitElement_);
  }
  if (ImGui::MenuItem("Show Comments", nullptr, commentsPresenter_.visible()))
    commentsPresenter_.setVisible(!commentsPresenter_.visible());
}

}  // namespace donner::editor
