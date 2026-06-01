#include "donner/editor/LayersPanel.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "donner/editor/ImGuiIncludes.h"
#include "donner/svg/ElementType.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/properties/PropertyRegistry.h"
#include "donner/svg/renderer/RenderElementToBitmap.h"
#include "misc/cpp/imgui_stdlib.h"

namespace donner::editor {

namespace {

/// Pixel size of the square preview cell drawn for each row.
constexpr int kPreviewSizePx = 24;

/// Draw a transparent-checkerboard background into the rect [min,max), so the
/// preview cell reads as "transparent canvas" behind a thumbnail's rendered
/// pixels (the standard alpha-checkerboard convention). Light/dark 6px squares,
/// clipped to the rounded cell by the caller's surrounding rect. This is plain
/// UI chrome (not a depiction of document geometry), so ImGui draws it.
void DrawCheckerboard(ImDrawList* drawList, const ImVec2& min, const ImVec2& max) {
  constexpr float kCell = 6.0f;
  constexpr ImU32 kLight = IM_COL32(120, 120, 120, 255);
  constexpr ImU32 kDark = IM_COL32(90, 90, 90, 255);
  drawList->AddRectFilled(min, max, kDark);
  int rowParity = 0;
  for (float y = min.y; y < max.y; y += kCell, ++rowParity) {
    const float cellMaxY = std::min(y + kCell, max.y);
    int col = rowParity & 1;
    for (float x = min.x; x < max.x; x += kCell, ++col) {
      if ((col & 1) == 0) {
        continue;
      }
      const float cellMaxX = std::min(x + kCell, max.x);
      drawList->AddRectFilled(ImVec2(x, y), ImVec2(cellMaxX, cellMaxY), kLight);
    }
  }
}

/// Neutral gray used for rows whose computed fill cannot be resolved to a solid
/// color (e.g. `none`, gradients/patterns, or `currentColor` without context).
constexpr css::RGBA kPlaceholderSwatch = css::RGBA(128, 128, 128, 255);

/// Resolve a deterministic preview swatch color from an element's computed fill.
///
/// The swatch is a solid-color UI rect (not a depiction of vector geometry) used
/// only as the fallback when no rendered thumbnail is available — e.g. a row
/// whose subtree has no boundable geometry, or a build with no GL texture
/// context. The real per-row preview is the Donner-rendered raster produced by
/// `svg::RenderElementToBitmap` in `refreshSnapshot`.
css::RGBA SwatchColorForElement(const svg::SVGElement& element) {
  const svg::PropertyRegistry& style = element.getComputedStyle();
  const auto fill = style.fill.get();
  if (!fill.has_value()) {
    return kPlaceholderSwatch;
  }
  if (!fill->is<svg::PaintServer::Solid>()) {
    return kPlaceholderSwatch;
  }
  const css::Color& color = fill->get<svg::PaintServer::Solid>().color;
  if (color.hasRGBA()) {
    return color.rgba();
  }
  return kPlaceholderSwatch;
}

ImU32 ToImU32(const css::RGBA& rgba) {
  return IM_COL32(rgba.r, rgba.g, rgba.b, rgba.a);
}

}  // namespace

void LayersPanel::refreshSnapshot(const EditorApp& app) {
  model_.refresh(app);

  swatchByStableId_.clear();
  thumbnailBitmapByStableId_.clear();
  for (const LayerTreeRow& row : model_.rows()) {
    swatchByStableId_.emplace(row.stableId, SwatchColorForElement(row.element));

    // Render the row's element subtree to a real RGBA thumbnail through the
    // Donner renderer (the same drawEntityRange path the compositor uses). Rows
    // whose subtree has no boundable geometry come back empty and fall back to
    // the swatch. A simple full rebuild on each refresh is acceptable for v1;
    // refreshSnapshot only runs when the renderer is idle.
    svg::RendererBitmap thumbnail =
        svg::RenderElementToBitmap(row.element, Vector2i(kPreviewSizePx, kPreviewSizePx));
    if (!thumbnail.empty()) {
      thumbnailBitmapByStableId_.emplace(row.stableId, std::move(thumbnail));
    }
  }

  // Drop a stale active row if it no longer maps to a visible row.
  if (activeRowIndex_.has_value() && *activeRowIndex_ >= model_.rows().size()) {
    activeRowIndex_.reset();
  }
  if (anchorRowIndex_.has_value() && *anchorRowIndex_ >= model_.rows().size()) {
    anchorRowIndex_.reset();
  }
}

std::optional<std::size_t> LayersPanel::rowIndexForStableId(std::uint64_t stableId) const {
  const std::vector<LayerTreeRow>& rows = model_.rows();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].stableId == stableId) {
      return i;
    }
  }
  return std::nullopt;
}

bool LayersPanel::hasThumbnailOrSwatch(std::uint64_t stableId) const {
  return swatchByStableId_.count(stableId) != 0;
}

std::optional<css::RGBA> LayersPanel::rowFallbackSwatch(std::uint64_t stableId) const {
  const auto it = swatchByStableId_.find(stableId);
  if (it == swatchByStableId_.end()) {
    return std::nullopt;
  }
  return it->second;
}

const svg::RendererBitmap* LayersPanel::rowThumbnail(std::uint64_t stableId) const {
  const auto it = thumbnailBitmapByStableId_.find(stableId);
  if (it == thumbnailBitmapByStableId_.end()) {
    return nullptr;
  }
  return &it->second;
}

bool LayersPanel::consumeSelectionChanged() {
  const bool changed = selectionChanged_;
  selectionChanged_ = false;
  return changed;
}

bool LayersPanel::consumeQueuedMutation() {
  const bool queued = mutationQueued_;
  mutationQueued_ = false;
  return queued;
}

void LayersPanel::noteRowHovered(std::optional<std::size_t> rowIndex) {
  const std::vector<LayerTreeRow>& rows = model_.rows();
  if (!rowIndex.has_value() || *rowIndex >= rows.size()) {
    hoveredElement_.reset();
    return;
  }
  hoveredElement_ = rows[*rowIndex].element;
}

void LayersPanel::handleRowClick(EditorApp& app, std::size_t rowIndex, ClickModifiers mods) {
  const std::vector<LayerTreeRow>& rows = model_.rows();
  if (rowIndex >= rows.size()) {
    return;
  }

  if (mods.shift && anchorRowIndex_.has_value() && *anchorRowIndex_ < rows.size()) {
    // Range select across the visible rows from the anchor to this row.
    const std::size_t lo = std::min(*anchorRowIndex_, rowIndex);
    const std::size_t hi = std::max(*anchorRowIndex_, rowIndex);
    std::vector<svg::SVGElement> rangeSelection;
    rangeSelection.reserve(hi - lo + 1);
    for (std::size_t i = lo; i <= hi; ++i) {
      rangeSelection.push_back(rows[i].element);
    }
    app.setSelection(std::move(rangeSelection));
  } else if (mods.ctrl) {
    app.toggleInSelection(rows[rowIndex].element);
    anchorRowIndex_ = rowIndex;
  } else {
    app.setSelection(rows[rowIndex].element);
    anchorRowIndex_ = rowIndex;
  }

  activeRowIndex_ = rowIndex;
  selectionChanged_ = true;
}

void LayersPanel::handleEyeClick(EditorApp& app, std::size_t rowIndex) {
  const std::vector<LayerTreeRow>& rows = model_.rows();
  if (rowIndex >= rows.size()) {
    return;
  }
  app.setElementVisible(rows[rowIndex].element, !rows[rowIndex].isVisible);
  // A show/hide toggle queues a DOM mutation but does not change the selection,
  // so it produces no `selectionChanged_` signal. Flag it separately so the
  // shell flushes the queued mutation and re-renders — otherwise the canvas
  // keeps showing the pre-toggle frame (the "hidden layer ghost" QA report).
  mutationQueued_ = true;
}

void LayersPanel::handleLockClick(EditorApp& app, std::size_t rowIndex) {
  const std::vector<LayerTreeRow>& rows = model_.rows();
  if (rowIndex >= rows.size()) {
    return;
  }
  app.setElementLocked(rows[rowIndex].element, !rows[rowIndex].isLocked);
  mutationQueued_ = true;
}

bool LayersPanel::handleRowRename(EditorApp& app, std::size_t rowIndex, std::string_view newId) {
  const std::vector<LayerTreeRow>& rows = model_.rows();
  if (rowIndex >= rows.size()) {
    return false;
  }
  // Select the row's element (you rename the thing you double-clicked), then run
  // the shared DOM-level rename engine, which also repoints references.
  app.setSelection(rows[rowIndex].element);
  selectionChanged_ = true;
  const bool renamed = app.renameSelectedElement(newId);
  mutationQueued_ = mutationQueued_ || renamed;
  return renamed;
}

bool LayersPanel::handleRowReorder(EditorApp& app, std::size_t fromIndex, std::size_t toIndex) {
  const std::vector<LayerTreeRow>& rows = model_.rows();
  if (fromIndex >= rows.size() || toIndex >= rows.size() || fromIndex == toIndex) {
    return false;
  }
  const svg::SVGElement moved = rows[fromIndex].element;
  const svg::SVGElement target = rows[toIndex].element;

  // Drop semantics on the flat row list: place `moved` at the target row's slot.
  // Dragging up (toIndex < fromIndex) inserts `moved` immediately before the
  // target; dragging down inserts it immediately after (i.e. before the target's
  // next sibling, or appends). reorderElementBeforeSibling rejects the move when
  // the two rows are not siblings of the same parent.
  std::optional<svg::SVGElement> referenceSibling;
  if (toIndex < fromIndex) {
    referenceSibling = target;
  } else {
    referenceSibling = target.nextSibling();
  }
  if (app.reorderElementBeforeSibling(moved, referenceSibling)) {
    app.setSelection(moved);
    selectionChanged_ = true;
    mutationQueued_ = true;
    return true;
  }
  return false;
}

bool LayersPanel::handleRowZOrder(EditorApp& app, std::size_t rowIndex,
                                  EditorApp::ZOrder direction) {
  const std::vector<LayerTreeRow>& rows = model_.rows();
  if (rowIndex >= rows.size()) {
    return false;
  }
  // Select the row's element, then run the shared paint-order engine (same path
  // as the canvas Arrange menu and the Cmd+[ / Cmd+] shortcuts).
  app.setSelection(rows[rowIndex].element);
  selectionChanged_ = true;
  const bool reordered = app.reorderSelectedElement(direction);
  mutationQueued_ = mutationQueued_ || reordered;
  return reordered;
}

void LayersPanel::beginRename(std::uint64_t stableId) {
  if (const auto rowIndex = rowIndexForStableId(stableId); rowIndex.has_value()) {
    renamingStableId_ = stableId;
    renameBuffer_ = model_.rows()[*rowIndex].displayName;
    renameFocusPending_ = true;
  }
}

void LayersPanel::render(EditorApp* liveApp, const ThumbnailTextureProvider& textureProvider) {
  const std::vector<LayerTreeRow>& rows = model_.rows();
  if (rows.empty()) {
    ImGui::TextDisabled("(no document)");
    return;
  }

  constexpr float kPreviewSize = static_cast<float>(kPreviewSizePx);
  constexpr float kIndentStep = 14.0f;
  ImDrawList* drawList = ImGui::GetWindowDrawList();

  std::optional<std::size_t> hoveredRowIndex;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const LayerTreeRow& row = rows[i];
    ImGui::PushID(static_cast<int>(static_cast<std::uint32_t>(row.stableId)));

    const float indent = static_cast<float>(row.depth) * kIndentStep;
    ImGui::Dummy(ImVec2(indent, 0.0f));
    ImGui::SameLine(0.0f, 0.0f);

    // Disclosure chevron for rows with children; clicking toggles expansion in
    // the owned model. A non-expandable row gets a same-width spacer so names
    // stay aligned.
    if (row.hasChildren) {
      const char* chevron = row.isExpanded ? "v" : ">";
      if (ImGui::SmallButton(chevron)) {
        model_.toggleExpanded(row.stableId);
      }
    } else {
      ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), 0.0f));
    }
    ImGui::SameLine();

    // 24x24 preview cell. The cell shows the row's Donner-rendered thumbnail
    // (the element subtree rasterized in refreshSnapshot, uploaded to an ImGui
    // texture by the shell-provided textureProvider and blitted via
    // ImGui::Image) over a transparent-canvas checkerboard. ImGui never draws
    // the vector geometry itself — only the Donner-produced texture. Rows with
    // no rendered thumbnail (or no GL texture context) fall back to the
    // deterministic fill swatch. See CLAUDE.md "No Rendering Vector Graphics
    // With ImGui".
    const ImVec2 swatchMin = ImGui::GetCursorScreenPos();
    const ImVec2 swatchMax(swatchMin.x + kPreviewSize, swatchMin.y + kPreviewSize);
    // Transparent-canvas checkerboard behind every preview cell, clipped to the
    // rounded cell so it does not bleed past the border drawn below.
    drawList->PushClipRect(swatchMin, swatchMax, /*intersect_with_current=*/true);
    DrawCheckerboard(drawList, swatchMin, swatchMax);
    drawList->PopClipRect();

    ImTextureID thumbnailTexture = 0;
    if (textureProvider) {
      if (const auto thumbnailIt = thumbnailBitmapByStableId_.find(row.stableId);
          thumbnailIt != thumbnailBitmapByStableId_.end()) {
        thumbnailTexture = textureProvider(row.stableId, thumbnailIt->second);
      }
    }

    if (thumbnailTexture != 0) {
      // Blit the Donner-rendered thumbnail texture over the checkerboard.
      drawList->AddImage(thumbnailTexture, swatchMin, swatchMax);
      drawList->AddRect(swatchMin, swatchMax, IM_COL32(255, 255, 255, 60), 3.0f);
    } else {
      // No rendered thumbnail available: draw the deterministic fill swatch over
      // the checkerboard. When the swatch is opaque it hides the checker; a
      // translucent/none fill lets the transparent backdrop show through.
      const auto swatchIt = swatchByStableId_.find(row.stableId);
      const css::RGBA swatch =
          swatchIt != swatchByStableId_.end() ? swatchIt->second : kPlaceholderSwatch;
      drawList->AddRectFilled(swatchMin, swatchMax, ToImU32(swatch), 3.0f);
      drawList->AddRect(swatchMin, swatchMax, IM_COL32(255, 255, 255, 60), 3.0f);
    }
    ImGui::Dummy(ImVec2(kPreviewSize, kPreviewSize));
    ImGui::SameLine();

    // Row name as a selectable. Partial selection gets a dimmer highlight.
    if (row.isPartiallySelected && !row.isSelected) {
      ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.35f));
    }
    bool selected = row.isSelected || row.isPartiallySelected;
    const bool isRenamingThisRow =
        renamingStableId_.has_value() && *renamingStableId_ == row.stableId;
    if (isRenamingThisRow) {
      // Inline rename: draw an edit field in place of the row label. Commit on
      // Enter (DOM-level rename via the shared engine), cancel on focus loss or
      // Escape. Donner owns the id change; ImGui only hosts the text field.
      if (renameFocusPending_) {
        ImGui::SetKeyboardFocusHere();
        renameFocusPending_ = false;
      }
      ImGui::SetNextItemWidth(-FLT_MIN);
      const bool committed = ImGui::InputText(
          "##layer_row_rename", &renameBuffer_,
          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
      const bool deactivated = ImGui::IsItemDeactivated();
      if (committed && liveApp != nullptr) {
        handleRowRename(*liveApp, i, renameBuffer_);
        renamingStableId_.reset();
      } else if (deactivated) {
        renamingStableId_.reset();  // Focus lost or Escape: abandon the edit.
      }
    } else {
      // AllowOverlap so the right-aligned eye/lock buttons (drawn after this
      // full-width Selectable) actually receive clicks — without it the
      // SpanAllColumns Selectable claims the whole row's hit area and the
      // buttons are dead.
      if (ImGui::Selectable(
              row.displayName.c_str(), selected,
              ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
        if (liveApp != nullptr) {
          ClickModifiers mods{
              .shift = ImGui::GetIO().KeyShift,
              .ctrl = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper,
          };
          handleRowClick(*liveApp, i, mods);
        }
      }
      // Track the hovered row so the canvas/source panes can highlight the
      // element under the cursor, mirroring source-pane hover. `IsItemHovered`
      // here refers to the row's selectable just drawn.
      if (ImGui::IsItemHovered()) {
        hoveredRowIndex = i;
        if (ImGui::IsMouseDoubleClicked(0) && liveApp != nullptr) {
          beginRename(row.stableId);  // Double-click to rename inline.
        }
      }

      // Drag-to-reorder: the row is a drag source carrying its visible index and
      // a drop target that issues a DOM move via the shared reorder engine.
      if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        const std::size_t payloadIndex = i;
        ImGui::SetDragDropPayload("DND_LAYER_ROW", &payloadIndex, sizeof(payloadIndex));
        ImGui::TextUnformatted(row.displayName.c_str());
        ImGui::EndDragDropSource();
      }
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DND_LAYER_ROW")) {
          std::size_t fromIndex = 0;
          std::memcpy(&fromIndex, payload->Data, sizeof(fromIndex));
          if (liveApp != nullptr) {
            handleRowReorder(*liveApp, fromIndex, i);
          }
        }
        ImGui::EndDragDropTarget();
      }
    }
    if (row.isPartiallySelected && !row.isSelected) {
      ImGui::PopStyleColor();
    }

    // Right-aligned per-row affordances: a visibility (eye) toggle and a lock
    // toggle. Drawn after the Selectable so they paint on top and capture
    // clicks before the row's selection hit area. Both funnel through the
    // shared handleEyeClick / handleLockClick seams (the same path the context
    // menu uses) and are dropped when the worker owns the document (liveApp ==
    // nullptr), mirroring the selection-click guard.
    const char* eyeIcon = row.isVisible ? "o" : "-";
    const char* lockIcon = row.isLocked ? "L" : "u";
    const ImGuiStyle& style = ImGui::GetStyle();
    const float eyeWidth = ImGui::CalcTextSize(eyeIcon).x + style.FramePadding.x * 2.0f;
    const float lockWidth = ImGui::CalcTextSize(lockIcon).x + style.FramePadding.x * 2.0f;
    const float affordancesWidth = eyeWidth + lockWidth + style.ItemSpacing.x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - affordancesWidth);
    if (ImGui::SmallButton(eyeIcon) && liveApp != nullptr) {
      handleEyeClick(*liveApp, i);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(lockIcon) && liveApp != nullptr) {
      handleLockClick(*liveApp, i);
    }

    // Right-click context menu with selection actions.
    if (ImGui::BeginPopupContextItem("##layer_row_menu")) {
      if (ImGui::MenuItem("Select") && liveApp != nullptr) {
        liveApp->setSelection(row.element);
        anchorRowIndex_ = i;
        selectionChanged_ = true;
      }
      if (ImGui::MenuItem("Select Group") && liveApp != nullptr) {
        // Select the nearest ancestor group, or the row itself if it is already
        // a group/root.
        svg::SVGElement target = row.element;
        if (row.kind != LayerRowKind::Group && row.kind != LayerRowKind::Root) {
          for (std::optional<svg::SVGElement> ancestor = row.element.parentElement();
               ancestor.has_value(); ancestor = ancestor->parentElement()) {
            const svg::ElementType type = ancestor->type();
            if (type == svg::ElementType::G || type == svg::ElementType::SVG) {
              target = *ancestor;
              break;
            }
          }
        }
        liveApp->setSelection(target);
        selectionChanged_ = true;
      }
      // Visibility + lock toggles, routed through the same shared seams as the
      // right-aligned row buttons so there is a single mutation path.
      if (row.isVisible) {
        if (ImGui::MenuItem("Hide") && liveApp != nullptr) {
          handleEyeClick(*liveApp, i);
        }
      } else {
        if (ImGui::MenuItem("Show") && liveApp != nullptr) {
          handleEyeClick(*liveApp, i);
        }
      }
      if (row.isLocked) {
        if (ImGui::MenuItem("Unlock") && liveApp != nullptr) {
          handleLockClick(*liveApp, i);
        }
      } else {
        if (ImGui::MenuItem("Lock") && liveApp != nullptr) {
          handleLockClick(*liveApp, i);
        }
      }
      // Rename starts the inline edit field on this row (Enter commits a
      // DOM-level id change through the shared engine). Disabled for locked rows,
      // matching the engine's own refusal.
      if (ImGui::MenuItem("Rename", nullptr, false, !row.isLocked) && liveApp != nullptr) {
        beginRename(row.stableId);
      }
      // Arrange (paint/z-order) — routes through the shared reorderSelectedElement
      // engine, same as the canvas Arrange menu and the Cmd+[ / Cmd+] shortcuts.
      if (ImGui::BeginMenu("Arrange", !row.isLocked)) {
        if (ImGui::MenuItem("Bring to Front", "Cmd+Shift+]") && liveApp != nullptr) {
          handleRowZOrder(*liveApp, i, EditorApp::ZOrder::BringToFront);
        }
        if (ImGui::MenuItem("Bring Forward", "Cmd+]") && liveApp != nullptr) {
          handleRowZOrder(*liveApp, i, EditorApp::ZOrder::BringForward);
        }
        if (ImGui::MenuItem("Send Backward", "Cmd+[") && liveApp != nullptr) {
          handleRowZOrder(*liveApp, i, EditorApp::ZOrder::SendBackward);
        }
        if (ImGui::MenuItem("Send to Back", "Cmd+Shift+[") && liveApp != nullptr) {
          handleRowZOrder(*liveApp, i, EditorApp::ZOrder::SendToBack);
        }
        ImGui::EndMenu();
      }
      // TODO(v0.8): Delete from the Layers panel — needs the in-sync source
      // text that lives in EditorShell, so it is wired at the shell level
      // rather than inventing a new EditorApp API here.
      ImGui::MenuItem("Delete", nullptr, false, false);
      ImGui::EndPopup();
    }

    ImGui::PopID();
  }

  noteRowHovered(hoveredRowIndex);

  // Keyboard navigation when the panel window is focused.
  if (ImGui::IsWindowFocused() && liveApp != nullptr && !rows.empty()) {
    std::size_t active = activeRowIndex_.value_or(0);
    active = std::min(active, rows.size() - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, /*repeat=*/true)) {
      active = std::min(active + 1, rows.size() - 1);
      activeRowIndex_ = active;
    } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, /*repeat=*/true)) {
      active = active == 0 ? 0 : active - 1;
      activeRowIndex_ = active;
    } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, /*repeat=*/false)) {
      const LayerTreeRow& row = rows[active];
      if (row.hasChildren && !row.isExpanded) {
        model_.setExpanded(row.stableId, true);
      }
    } else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, /*repeat=*/false)) {
      const LayerTreeRow& row = rows[active];
      if (row.hasChildren && row.isExpanded) {
        model_.setExpanded(row.stableId, false);
      } else if (const std::optional<svg::SVGElement> parent = row.element.parentElement();
                 parent.has_value()) {
        if (const auto parentIdx = rowIndexForStableId(LayerTreeModel::StableIdFor(*parent));
            parentIdx.has_value()) {
          activeRowIndex_ = *parentIdx;
        }
      }
    } else if (ImGui::IsKeyPressed(ImGuiKey_Enter, /*repeat=*/false) ||
               ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, /*repeat=*/false)) {
      liveApp->setSelection(rows[active].element);
      anchorRowIndex_ = active;
      selectionChanged_ = true;
    }
  }
}

}  // namespace donner::editor
