#include "donner/editor/LayersPanel.h"

#include <algorithm>
#include <string>
#include <vector>

#include "donner/editor/ImGuiIncludes.h"
#include "donner/svg/ElementType.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::editor {

namespace {

/// Neutral gray used for rows whose computed fill cannot be resolved to a solid
/// color (e.g. `none`, gradients/patterns, or `currentColor` without context).
constexpr css::RGBA kPlaceholderSwatch = css::RGBA(128, 128, 128, 255);

/// Resolve a deterministic preview swatch color from an element's computed fill.
///
/// A true per-row subtree-render thumbnail is deferred: design doc 0046 makes
/// "per-tier previews" its own follow-up milestone (Milestone 3 there), and a
/// faithful offscreen render requires wiring the renderer's entity-range
/// machinery and a texture cache into this panel. For v0.8 the Layers panel
/// instead shows a deterministic swatch derived from the element's computed
/// `fill`, falling back to a neutral gray. This guarantees every visible row
/// has a non-empty preview cell.
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
  for (const LayerTreeRow& row : model_.rows()) {
    swatchByStableId_.emplace(row.stableId, SwatchColorForElement(row.element));
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

bool LayersPanel::consumeSelectionChanged() {
  const bool changed = selectionChanged_;
  selectionChanged_ = false;
  return changed;
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

void LayersPanel::render(EditorApp* liveApp) {
  const std::vector<LayerTreeRow>& rows = model_.rows();
  if (rows.empty()) {
    ImGui::TextDisabled("(no document)");
    return;
  }

  constexpr float kPreviewSize = 24.0f;
  constexpr float kIndentStep = 14.0f;
  ImDrawList* drawList = ImGui::GetWindowDrawList();

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

    // Eye icon placeholder.
    // TODO(v0.8): visibility toggle — wire to a hide/show editor command. No-op
    // for now so the affordance is visible without risking source desync.
    ImGui::TextUnformatted(row.isVisible ? "o" : "-");
    ImGui::SameLine();

    // 24x24 preview swatch (deterministic fill-derived fallback; see
    // SwatchColorForElement for why a real subtree render is deferred).
    const ImVec2 swatchMin = ImGui::GetCursorScreenPos();
    const ImVec2 swatchMax(swatchMin.x + kPreviewSize, swatchMin.y + kPreviewSize);
    const auto swatchIt = swatchByStableId_.find(row.stableId);
    const css::RGBA swatch =
        swatchIt != swatchByStableId_.end() ? swatchIt->second : kPlaceholderSwatch;
    drawList->AddRectFilled(swatchMin, swatchMax, ToImU32(swatch), 3.0f);
    drawList->AddRect(swatchMin, swatchMax, IM_COL32(255, 255, 255, 60), 3.0f);
    ImGui::Dummy(ImVec2(kPreviewSize, kPreviewSize));
    ImGui::SameLine();

    // Row name as a selectable. Partial selection gets a dimmer highlight.
    if (row.isPartiallySelected && !row.isSelected) {
      ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.35f));
    }
    bool selected = row.isSelected || row.isPartiallySelected;
    if (ImGui::Selectable(row.displayName.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
      if (liveApp != nullptr) {
        ClickModifiers mods{
            .shift = ImGui::GetIO().KeyShift,
            .ctrl = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper,
        };
        handleRowClick(*liveApp, i, mods);
      }
    }
    if (row.isPartiallySelected && !row.isSelected) {
      ImGui::PopStyleColor();
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
      // TODO(v0.8): Hide/Show visibility toggles — no-op until a hide/show
      // editor command exists.
      ImGui::MenuItem("Hide", nullptr, false, false);
      ImGui::MenuItem("Show", nullptr, false, false);
      // TODO(v0.8): Delete from the Layers panel — needs the in-sync source
      // text that lives in EditorShell, so it is wired at the shell level
      // rather than inventing a new EditorApp API here.
      ImGui::MenuItem("Delete", nullptr, false, false);
      ImGui::EndPopup();
    }

    ImGui::PopID();
  }

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
