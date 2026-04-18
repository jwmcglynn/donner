#pragma once
/// @file

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/ViewportState.h"

namespace donner::editor {

class SelectTool;

struct TreeViewState {
  std::optional<svg::SVGElement> scrollTarget;
  bool pendingScroll = false;
  bool selectionChangedInTree = false;
};

/// Renders the editor's tree view and inspector panes.
///
/// The panes are always rendered from an internal snapshot so they stay
/// visible even while the async renderer is mutating the document (the
/// "(rendering…)" placeholder used to cover this gap, which made the panes
/// flash to a disabled message on every render). The snapshot is refreshed
/// from the live `EditorApp` when the caller indicates the worker thread
/// isn't touching the document; otherwise the most recent capture is
/// replayed unchanged. Click handling is gated the same way so mutations
/// can't race the worker.
class SidebarPresenter {
public:
  /// Refresh the tree / inspector snapshot from live app state. Safe to
  /// call only when the async renderer is idle.
  void refreshSnapshot(const EditorApp& app);

  /// Render the tree pane from the current snapshot. When @p liveApp is
  /// non-null, click-induced selection mutations are applied to it; when
  /// null, clicks are dropped (the render is "read-only" because the worker
  /// thread owns the document). `selectTool` (if non-null) is given a
  /// chance to commit any deferred drag mutation before the selection
  /// actually changes — otherwise the fresh render that fires after the
  /// click would still see the pre-drag DOM transform on the last drag
  /// target.
  void renderTreeView(EditorApp* liveApp, SelectTool* selectTool,
                      TreeViewState& state) const;

  /// Render the inspector pane from the current snapshot.
  void renderInspector(const ViewportState& viewport) const;

private:
  struct TreeNodeSnapshot {
    /// Captured element reference. Valid for as long as the underlying
    /// entity isn't destroyed — for light-tree nodes that only happens
    /// on a full document rebuild (`resetAllLayers` / document reload),
    /// at which point the snapshot is refreshed on the next idle frame.
    std::optional<svg::SVGElement> element;
    std::string label;
    bool isSelected = false;
    std::vector<TreeNodeSnapshot> children;
  };

  struct InspectorSnapshot {
    bool hasSelection = false;
    std::string titleText;
    std::optional<Box2d> bounds;
    std::optional<Transform2d> transform;
  };

  void captureTreeNode(const svg::SVGElement& element,
                        std::span<const svg::SVGElement> selection,
                        TreeNodeSnapshot& out);
  void renderTreeNode(EditorApp* liveApp, SelectTool* selectTool,
                      const TreeNodeSnapshot& node, TreeViewState& state) const;

  std::optional<TreeNodeSnapshot> treeSnapshot_;
  InspectorSnapshot inspectorSnapshot_;
};

}  // namespace donner::editor
