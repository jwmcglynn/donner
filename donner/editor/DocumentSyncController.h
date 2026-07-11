#pragma once
/// @file

#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "donner/base/ParseDiagnostic.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/SourceDiagnostics.h"
#include "donner/editor/SourceEditIntent.h"
#include "donner/editor/TextEditor.h"

namespace donner::editor {

class SelectTool;

/// Owns source-pane debounce, parse-error markers, and XML-owned source view mirroring.
class DocumentSyncController {
public:
  explicit DocumentSyncController(std::string initialSource);

  void resetForLoadedDocument(const std::string& source);

  void syncParseErrorMarkers(EditorApp& app, TextEditor& textEditor);
  /// Diagnostics normalized for the current source buffer and parse revision.
  [[nodiscard]] const SourceDiagnosticSnapshot& sourceDiagnostics() const {
    return sourceDiagnostics_;
  }
  /**
   * Mirror XML-owned source deltas into the source pane.
   *
   * Use this when a canvas/DOM operation already mutated the XML document and returned
   * \ref xml::XMLSourceDelta records. The controller replays the deltas into \p textEditor with
   * source-change suppression and falls back to mirroring the document source if the delta sequence
   * cannot be applied precisely.
   *
   * @param app Editor app containing the source-backed document.
   * @param textEditor Source pane to update.
   * @param sourceDeltas Source deltas emitted by the XML document.
   * @return True if the source pane was updated from the deltas or fallback mirror.
   */
  bool mirrorSourceDeltas(EditorApp& app, TextEditor& textEditor,
                          const std::vector<xml::XMLSourceDelta>& sourceDeltas);
  void handleTextEdits(EditorApp& app, TextEditor& textEditor, float deltaSeconds);
  /// Return the pending source-text sync wake interval, if a throttled edit is waiting.
  [[nodiscard]] std::optional<float> nextTextSyncWakeSeconds() const;
  void applyPendingWritebacks(EditorApp& app, SelectTool& selectTool, TextEditor& textEditor);

private:
  std::string previousSourceText_;
  std::optional<std::string> lastWritebackSourceText_;
  /// Pending transform writebacks that haven't been reflected in the source
  /// pane yet. Source-backed documents route these through XML DOM mutation;
  /// source-less fallbacks still use legacy text patches.
  std::vector<EditorApp::CompletedTransformWriteback> pendingTransformWritebacks_;
  std::vector<EditorApp::CompletedElementRemoveWriteback> pendingElementRemoveWritebacks_;
  std::vector<SourceEditIntent> pendingSourceEditIntents_;

  SourceDiagnosticSnapshot sourceDiagnostics_;
  std::uint64_t lastSyncedDiagnosticsRevision_ = std::numeric_limits<std::uint64_t>::max();

  bool textChangePending_ = false;
  bool textDispatchThrottled_ = false;
  float textChangeIdleTimer_ = 0.0f;
};

}  // namespace donner::editor
