#pragma once
/// @file

#include <optional>
#include <span>

#include "donner/editor/EditorApp.h"
#include "donner/editor/ViewportState.h"

namespace donner::editor {

struct TreeViewState {
  std::optional<svg::SVGElement> scrollTarget;
  bool pendingScroll = false;
  bool selectionChangedInTree = false;
};

/// Renders the advanced editor's tree view and inspector panes.
class SidebarPresenter {
public:
  void renderTreeView(EditorApp& app, TreeViewState& state) const;
  void renderInspector(const EditorApp& app, const ViewportState& viewport) const;
};

}  // namespace donner::editor
