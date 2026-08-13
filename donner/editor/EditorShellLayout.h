#pragma once
/// @file

namespace donner::editor {

/// Editor chrome profile selected from viewport constraints and input type.
enum class EditorUiMode {
  Desktop,       ///< Persistent desktop menus, source access, and docked panels.
  CompactTouch,  ///< Canvas-first chrome with touch-sized controls and one panel sheet.
};

/// Edge used for the compact Layers or Inspector sheet.
enum class CompactPanelPlacement {
  Right,   ///< Landscape viewport: panel overlays the right edge.
  Bottom,  ///< Portrait or square viewport: panel overlays the bottom edge.
};

/// Inputs used to select and size the adaptive editor chrome.
struct EditorAdaptiveUiInput {
  /// Full editor window width in logical screen pixels.
  float windowWidth = 0.0f;
  /// Full editor window height in logical screen pixels.
  float windowHeight = 0.0f;
  /// Whether the current platform or pointer source should prefer touch controls.
  bool preferTouch = false;
};

/// Adaptive editor chrome geometry and feature subset for one frame.
struct EditorAdaptiveUiLayout {
  /// Selected desktop or compact-touch profile.
  EditorUiMode mode = EditorUiMode::Desktop;
  /// Edge used by the compact panel sheet.
  CompactPanelPlacement panelPlacement = CompactPanelPlacement::Right;
  /// Height reserved for compact top-level commands. Zero in desktop mode.
  float topBarHeight = 0.0f;
  /// Square tool-button size in logical pixels.
  float toolButtonSize = 32.0f;
  /// Compact sheet origin and size. Zero-sized in desktop mode.
  float panelX = 0.0f;
  float panelY = 0.0f;
  float panelWidth = 0.0f;
  float panelHeight = 0.0f;
  /// Whether the canvas palette includes the combined fill/stroke control.
  bool showPaintControls = true;
  /// Whether the contextual text format bar is available.
  bool showTextFormatBar = true;
  /// Whether mouse-oriented canvas scrollbars are drawn.
  bool showCanvasScrollbars = true;

  /// Return true when the compact touch profile is active.
  [[nodiscard]] bool compactTouch() const { return mode == EditorUiMode::CompactTouch; }
};

/**
 * Select and size the editor's desktop or compact-touch chrome.
 *
 * Constrained native windows use the compact profile even before a touch event.
 * Touch-preferred platforms use it at every size so controls never shrink after
 * the first gesture.
 *
 * @param input Window geometry and input preference.
 */
[[nodiscard]] EditorAdaptiveUiLayout ComputeEditorAdaptiveUiLayout(
    const EditorAdaptiveUiInput& input);

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
  /// Width of the persistent source reveal rail while the pane is hidden.
  float sourcePaneRailWidth = 0.0f;
  /// Persisted/requested right sidebar width.
  float rightPaneWidth = 0.0f;
  /// Whether the persistent right sidebar participates in the layout.
  bool rightPaneVisible = true;
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
  /// Effective source reveal-rail width. Zero while the source pane is visible.
  float sourcePaneRailWidth = 0.0f;
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

/// A screen-space rectangle in logical pixels.
struct LayoutRect {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
};

/// Geometry the render pane's first-fit latch inspects to decide whether this frame's pane
/// rectangle is the settled one. See \ref RenderPaneViewportLatchReady.
struct RenderPaneLatchInput {
  /// Rect of the docked render-pane window as ImGui reports it this frame.
  LayoutRect paneWindow;
  /// Rect of the DockSpace central node as of this frame's dock-host pass.
  LayoutRect dockCentralNode;
  /// Dock host rect the shell computed for this frame. Derived from the window size and the
  /// shell's own pane state, never from ImGui's layout state.
  LayoutRect dockHost;
  /// The same shell-computed dock host rect from the previous frame.
  LayoutRect previousDockHost;
  /// Render-pane content region this frame.
  float paneContentWidth = 0.0f;
  float paneContentHeight = 0.0f;
  /// Render-pane content region on the previous frame, or a negative size before any frame has
  /// reported one.
  float previousPaneContentWidth = -1.0f;
  float previousPaneContentHeight = -1.0f;
  /// Whether this frame's dock layout splits a sidebar column off the host. False in the
  /// compact-touch profile, whose root node *is* the canvas node.
  bool sidebarColumnIncluded = true;
};

/**
 * Decide whether the render pane's rectangle may be latched as the document's initial fit.
 *
 * The canvas is docked into the DockSpace central node, so ImGui owns the pane rectangle and
 * reports transient values while a layout change propagates. Fitting the document to a transient
 * rectangle and rendering against it presents the document at the wrong fit (the placeholder
 * viewport) and then jumps once the pane settles, so the latch must reject every pre-settle
 * rectangle regardless of how many frames the transient survives.
 *
 * The policy is two-sided and grounded in geometry the shell computes itself:
 *
 * 1. The dock host rect, a pure function of the window size and the shell's own pane state, must
 *    be unchanged from the previous frame. Nothing downstream of a moving host is settled, and no
 *    inspection of ImGui's own state can tell a moving host from a stationary one.
 * 2. ImGui must have propagated this frame's central-node rect into the docked pane window, i.e.
 *    the pane window rect equals the central node rect. A stale pane window fails this whether it
 *    is larger or smaller than the node.
 * 3. The central node must be the host's column split: same origin, full host height, and never
 *    wider than the host. These are exact comparisons against the shell's own host rect, so a node
 *    that is short or offset because some upstream geometry has not settled is rejected.
 * 4. When the layout carries a sidebar column, that column must already be split off (the node is
 *    strictly narrower than the host); in the compact profile the node instead spans the host.
 *
 * A dock tree this policy cannot recognize, a customized layout restored from the persisted .ini
 * say, falls back to the pre-existing rule: the same content region on two consecutive frames,
 * still gated on a stationary host.
 *
 * @param input This frame's pane, central-node, and host geometry plus the previous frame's.
 */
[[nodiscard]] bool RenderPaneViewportLatchReady(const RenderPaneLatchInput& input);

}  // namespace donner::editor
