#pragma once
/// @file

#include <optional>
#include <string>
#include <vector>

#include "donner/base/ParseDiagnostic.h"
#include "donner/editor/backend_lib/AttributeWriteback.h"
#include "donner/editor/backend_lib/EditorApp.h"
#include "donner/editor/TextEditor.h"

namespace donner::editor {

class SelectTool;

/// Owns source-pane debounce, parse-error markers, and pending canvas→text writebacks.
class DocumentSyncController {
public:
  explicit DocumentSyncController(std::string initialSource);

  void resetForLoadedDocument(const std::string& source);

  void syncParseErrorMarkers(EditorApp& app, TextEditor& textEditor);
  void handleTextEdits(EditorApp& app, TextEditor& textEditor, float deltaSeconds);
  void applyPendingWritebacks(EditorApp& app, SelectTool& selectTool, TextEditor& textEditor);

private:
  static TextEditor::ErrorMarkers ParseErrorToMarkers(const ParseDiagnostic& diag);

  std::string previousSourceText_;
  std::optional<std::string> lastWritebackSourceText_;
  /// Pending transform writebacks that haven't been reflected in the source
  /// pane yet. Was a single `optional`; became a vector to support multi-
  /// element drag (each dragged element produces its own writeback, and
  /// they're all applied in the same source patch pass to avoid flashing).
  std::vector<EditorApp::CompletedTransformWriteback> pendingTransformWritebacks_;
  std::vector<EditorApp::CompletedElementRemoveWriteback> pendingElementRemoveWritebacks_;

  int lastShownErrorLine_ = -1;
  std::string lastShownErrorReason_;

  bool textChangePending_ = false;
  bool textDispatchThrottled_ = false;
  float textChangeIdleTimer_ = 0.0f;
};

}  // namespace donner::editor
