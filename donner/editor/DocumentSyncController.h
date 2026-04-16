#pragma once
/// @file

#include <optional>
#include <string>
#include <vector>

#include "donner/base/ParseDiagnostic.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorApp.h"
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
  std::optional<EditorApp::CompletedTransformWriteback> pendingTransformWriteback_;
  std::vector<EditorApp::CompletedElementRemoveWriteback> pendingElementRemoveWritebacks_;

  int lastShownErrorLine_ = -1;
  std::string lastShownErrorReason_;

  bool textChangePending_ = false;
  bool textDispatchThrottled_ = false;
  float textChangeIdleTimer_ = 0.0f;
};

}  // namespace donner::editor
