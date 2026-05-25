#pragma once
/// @file

namespace donner::editor {

/// Inputs used to compute the editor's horizontal source/render/sidebar layout.
struct EditorMainPaneLayoutInput {
  /// Full editor window width in screen pixels.
  float windowWidth = 0.0f;
  /// Whether the source pane is currently visible.
  bool sourcePaneVisible = true;
  /// Preferred source pane width when visible.
  float sourcePaneWidth = 0.0f;
  /// Minimum source pane width while visible.
  float minSourcePaneWidth = 0.0f;
  /// Maximum source pane width while visible.
  float maxSourcePaneWidth = 0.0f;
  /// Persisted/requested right sidebar width.
  float rightPaneWidth = 0.0f;
  /// Minimum right sidebar width.
  float minRightPaneWidth = 0.0f;
  /// Maximum right sidebar width.
  float maxRightPaneWidth = 0.0f;
  /// Minimum render pane width to preserve before growing the right sidebar.
  float minRenderPaneWidth = 0.0f;
};

/// Computed geometry for the editor's horizontal source/render/sidebar layout.
struct EditorMainPaneLayout {
  /// Effective source pane width. Zero when the source pane is hidden.
  float sourcePaneWidth = 0.0f;
  /// Clamped right sidebar width.
  float rightPaneWidth = 0.0f;
  /// Left edge of the render pane.
  float renderPaneX = 0.0f;
  /// Width of the render pane.
  float renderPaneWidth = 0.0f;
  /// Left edge of the right sidebar.
  float rightPaneX = 0.0f;
};

/**
 * Compute horizontal pane geometry for the source pane, render pane, and
 * right sidebar.
 *
 * @param input Horizontal layout constraints and persisted sidebar width.
 */
[[nodiscard]] EditorMainPaneLayout ComputeEditorMainPaneLayout(
    const EditorMainPaneLayoutInput& input);

/// Inputs used to compute the editor's right sidebar pane layout.
struct RightSidebarLayoutInput {
  /// Top of the sidebar content region in screen pixels.
  float paneOriginY = 0.0f;
  /// Total height available below the menu bar in screen pixels.
  float paneHeight = 0.0f;
  /// Vertical gap between the tree pane and inspector pane.
  float rightPaneGap = 0.0f;
  /// Fraction of \ref paneHeight assigned to the tree view.
  float treeViewHeightFraction = 0.0f;
  /// Fraction of the inspector/layer budget assigned to the layer panel.
  float layerPanelHeightFraction = 0.0f;
  /// Whether the layer panel is rendered as an independent floating window.
  bool layerPanelDetached = false;
  /// Height of the draggable splitter above the layer panel.
  float layerPanelSplitterThickness = 0.0f;
  /// Preferred minimum height for the compositor layer panel.
  float minLayerPanelHeight = 0.0f;
  /// Preferred minimum height for the inspector pane.
  float minInspectorPaneHeight = 0.0f;
};

/// Computed geometry for the editor's right sidebar panes.
struct RightSidebarLayout {
  /// Height of the XML tree pane.
  float treePaneHeight = 0.0f;
  /// Top of the inspector pane in screen pixels.
  float inspectorPaneY = 0.0f;
  /// Height of the inspector pane.
  float inspectorPaneHeight = 0.0f;
  /// Top of the draggable splitter above the layer panel.
  float layerPanelSplitterY = 0.0f;
  /// Top of the compositor layer panel in screen pixels.
  float layerPanelPaneY = 0.0f;
  /// Height of the compositor layer panel.
  float layerPanelHeight = 0.0f;
  /// Combined height available to the inspector, splitter, and layer panel.
  float lowerPaneHeight = 0.0f;
  /// Effective minimum layer panel height after clamping to the available budget.
  float minLayerPanelHeight = 0.0f;
  /// Effective maximum layer panel height after preserving the inspector minimum.
  float maxLayerPanelHeight = 0.0f;
  /// Normalized layer panel height fraction after min/max clamping.
  float layerPanelHeightFraction = 0.0f;
};

/**
 * Compute right sidebar pane geometry for a fixed tree view and resizable
 * inspector/layer split.
 *
 * @param input Sidebar layout constraints and persisted layer split fraction.
 */
[[nodiscard]] RightSidebarLayout ComputeRightSidebarLayout(const RightSidebarLayoutInput& input);

/**
 * Update the layer panel height fraction from a vertical splitter drag.
 *
 * @param currentFraction Current fraction of \p lowerPaneHeight assigned to the layer panel.
 * @param lowerPaneHeight Height available to the inspector and layer panel.
 * @param minLayerPanelHeight Minimum allowed layer panel height.
 * @param maxLayerPanelHeight Maximum allowed layer panel height.
 * @param splitterDeltaY ImGui mouse delta for the splitter; dragging down shrinks the layer panel.
 */
[[nodiscard]] float ResizeLayerPanelHeightFraction(float currentFraction, float lowerPaneHeight,
                                                   float minLayerPanelHeight,
                                                   float maxLayerPanelHeight, float splitterDeltaY);

}  // namespace donner::editor
