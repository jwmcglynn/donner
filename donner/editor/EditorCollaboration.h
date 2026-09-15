#pragma once
/// @file

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/Vector2.h"
#include "donner/editor/EditorApp.h"
#include "nlohmann/json.hpp"

namespace donner::editor {

/// Feedback anchored to an element and a document-space point, outside the SVG artwork.
struct EditorComment {
  std::uint64_t id = 0;
  std::string documentKey;
  std::string createdSessionId;
  std::uint64_t createdSourceRevision = 0;
  Vector2d documentPoint;
  Vector2d presentedPoint;
  std::optional<Vector2d> elementPoint;
  bool orphaned = false;
  std::string elementId;
  std::string elementLabel;
  std::string text;
  bool resolved = false;
};

/// UI-thread controller shared by the visible editor and its local MCP connection.
class EditorCollaboration {
public:
  /// Frame integration uses the same flush/save paths as ordinary editor actions.
  struct Callbacks {
    std::function<bool()> flush;
    std::function<bool(std::string*)> save;
  };

  /// Attach to the existing editor document; this controller never owns a second document.
  EditorCollaboration(EditorApp& app, Callbacks callbacks);

  /// Handle one bounded MCP request on the UI thread, while the renderer is idle.
  nlohmann::json handleRequest(const nlohmann::json& request);

  /// Add feedback from the context menu. Returns an error for empty or oversized text.
  /// Capture an immutable click anchor before a user starts composing feedback.
  std::optional<EditorComment> captureCommentAnchor(Vector2d point,
                                                    std::optional<svg::SVGElement> element);
  /// Submit feedback against the captured click, preserving its local coordinates across edits.
  bool addAnchoredComment(EditorComment anchor, std::string text, std::string* error);
  bool addComment(Vector2d point, std::optional<svg::SVGElement> element, std::string text,
                  std::string* error);

  /// All retained comments; the presenter filters by documentGeneration.
  const std::vector<EditorComment>& comments() const { return comments_; }
  /// Mark a comment resolved or reopen it.
  bool setCommentResolved(std::uint64_t id, bool resolved);
  /// Current open-document generation used to keep feedback scoped to its original document.
  std::uint64_t documentGeneration() const;
  /// True when feedback belongs to the file or untitled document currently open.
  bool isCurrentComment(const EditorComment& comment) const;
  /// Refresh element-relative anchors at a safe frame boundary.
  void refreshCommentAnchors();
  /// Refresh a retained or draft anchor at a safe frame boundary.
  /// @param comment Anchor whose presented position follows the referenced element.
  void refreshCommentAnchor(EditorComment& comment);
  /// Feedback across retained document sessions for private persistence.
  nlohmann::json feedbackArchive() const;
  /// Monotonic feedback cursor for event-driven clients.
  std::uint64_t feedbackRevision() const { return feedbackRevision_; }
  /// Whether a valid feedback wait should remain queued until another UI event.
  bool shouldWaitForFeedback(const nlohmann::json& request) const;
  /// Whether this request reads or changes the live SVG and must wait for an idle frame.
  static bool requiresIdleDocument(const nlohmann::json& request);

  /// Persistable feedback data, separate from SVG source and document undo history.
  nlohmann::json feedback() const;
  /// Restore validated feedback belonging to the current source file.
  bool restoreFeedback(const nlohmann::json& data);
  /// True when feedback has changed since the last persistence checkpoint.
  bool feedbackDirty() const { return feedbackDirty_; }
  /// Mark the current feedback checkpoint as persisted.
  void markFeedbackClean() { feedbackDirty_ = false; }

private:
  nlohmann::json callTool(std::string_view name, const nlohmann::json& arguments);
  nlohmann::json state();
  nlohmann::json inspect(const svg::SVGElement& element);
  nlohmann::json applyEdits(const nlohmann::json& arguments);
  nlohmann::json insertElement(const nlohmann::json& arguments);
  std::optional<std::string> checkContext(const nlohmann::json& arguments);
  std::optional<std::string> checkRevision(const nlohmann::json& arguments);
  nlohmann::json resolveCommentTool(const nlohmann::json& arguments);
  nlohmann::json waitCommentsTool(const nlohmann::json& arguments);
  nlohmann::json historyTool(std::string_view name, const nlohmann::json& arguments);
  nlohmann::json sourceTool(const nlohmann::json& arguments);
  nlohmann::json pickTool(const nlohmann::json& arguments);
  nlohmann::json selectionTool(const nlohmann::json& arguments, bool deleting);
  nlohmann::json dispatchToolRequest(const nlohmann::json& request, std::string* error);
  static nlohmann::json toolList();
  std::string documentKey() const;

  EditorApp& app_;
  std::string sessionId_;
  Callbacks callbacks_;
  std::vector<EditorComment> comments_;
  std::uint64_t nextCommentId_ = 1;
  bool feedbackDirty_ = false;
  std::uint64_t feedbackRevision_ = 0;
  std::optional<std::uint64_t> anchorFrameVersion_;
};

}  // namespace donner::editor
