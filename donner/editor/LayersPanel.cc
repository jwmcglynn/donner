#include "donner/editor/LayersPanel.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "donner/base/Path.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/svg/ElementType.h"
#include "donner/svg/SVGGeometryElement.h"
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

/// True for element types whose computed outline we can sample into a real
/// per-shape thumbnail via `SVGGeometryElement::computedSpline()`.
bool IsGeometryThumbnailType(svg::ElementType type) {
  switch (type) {
    case svg::ElementType::Path:
    case svg::ElementType::Rect:
    case svg::ElementType::Circle:
    case svg::ElementType::Ellipse:
    case svg::ElementType::Line:
    case svg::ElementType::Polyline:
    case svg::ElementType::Polygon: return true;
    default: return false;
  }
}

/// Resolve a computed paint to a solid RGBA, or `std::nullopt` when it is
/// `none`/unresolved or a non-solid paint (gradient/pattern).
std::optional<css::RGBA> SolidPaintColor(const svg::PropertyRegistry& style, bool fillSlot) {
  const auto paint = fillSlot ? style.fill.get() : style.stroke.get();
  if (!paint.has_value() || !paint->is<svg::PaintServer::Solid>()) {
    return std::nullopt;
  }
  const css::Color& color = paint->get<svg::PaintServer::Solid>().color;
  if (!color.hasRGBA()) {
    return std::nullopt;
  }
  return color.rgba();
}

/// Build a real geometry thumbnail for @p element: its computed outline sampled
/// into a polyline normalized into the unit square, plus its computed
/// fill/stroke. Returns `std::nullopt` for non-geometry elements or when the
/// outline is degenerate (no area), so the caller falls back to the swatch.
std::optional<LayersPanel::RowThumbnail> BuildRowThumbnail(const svg::SVGElement& element) {
  const std::optional<svg::ElementType> type = element.tryType();
  if (!type.has_value() || !IsGeometryThumbnailType(*type)) {
    return std::nullopt;
  }

  const svg::SVGGeometryElement geometry = element.cast<svg::SVGGeometryElement>();
  const std::optional<Path> spline = geometry.computedSpline();
  if (!spline.has_value() || spline->empty()) {
    return std::nullopt;
  }

  // Walk the path verbs collecting on-path endpoints only (the destination point
  // of each MoveTo/LineTo/QuadTo/CurveTo), skipping Bézier control points so the
  // silhouette traces the shape's outline rather than its control hull. Record
  // whether any subpath closes for the fill/stroke decision below.
  std::vector<Vector2d> points;
  bool closedPath = false;
  spline->forEach([&](Path::Verb verb, std::span<const Vector2d> verbPoints) {
    if (verb == Path::Verb::ClosePath) {
      closedPath = true;
      return;
    }
    if (!verbPoints.empty()) {
      points.push_back(verbPoints.back());
    }
  });
  if (points.size() < 2) {
    return std::nullopt;
  }

  // Normalize the outline into the unit square, preserving aspect ratio and
  // centering, so every row's silhouette fills its preview cell consistently.
  Vector2d minPoint = points.front();
  Vector2d maxPoint = points.front();
  for (const Vector2d& point : points) {
    minPoint.x = std::min(minPoint.x, point.x);
    minPoint.y = std::min(minPoint.y, point.y);
    maxPoint.x = std::max(maxPoint.x, point.x);
    maxPoint.y = std::max(maxPoint.y, point.y);
  }
  const Vector2d extent = maxPoint - minPoint;
  const double span = std::max(extent.x, extent.y);
  if (span <= 0.0) {
    return std::nullopt;
  }
  const Vector2d offset((1.0 - extent.x / span) * 0.5, (1.0 - extent.y / span) * 0.5);

  LayersPanel::RowThumbnail thumbnail;
  thumbnail.normalizedPoints.reserve(points.size());
  for (const Vector2d& point : points) {
    thumbnail.normalizedPoints.push_back(Vector2d((point.x - minPoint.x) / span + offset.x,
                                                  (point.y - minPoint.y) / span + offset.y));
  }

  const svg::PropertyRegistry& style = element.getComputedStyle();
  const std::optional<css::RGBA> fill = SolidPaintColor(style, /*fillSlot=*/true);
  const std::optional<css::RGBA> stroke = SolidPaintColor(style, /*fillSlot=*/false);
  thumbnail.fill = fill.value_or(css::RGBA(0, 0, 0, 0));
  // When the shape has a real fill, treat the silhouette as a closed filled
  // region; otherwise draw it as an outline using the stroke (or a neutral line
  // when neither is solid) so stroke-only shapes stop collapsing to a flat box.
  thumbnail.closed = fill.has_value() && fill->a > 0;
  thumbnail.stroke = stroke.value_or(fill.value_or(css::RGBA(200, 200, 200, 255)));
  return thumbnail;
}

}  // namespace

void LayersPanel::refreshSnapshot(const EditorApp& app) {
  model_.refresh(app);

  swatchByStableId_.clear();
  thumbnailByStableId_.clear();
  for (const LayerTreeRow& row : model_.rows()) {
    swatchByStableId_.emplace(row.stableId, SwatchColorForElement(row.element));
    if (std::optional<RowThumbnail> thumbnail = BuildRowThumbnail(row.element);
        thumbnail.has_value()) {
      thumbnailByStableId_.emplace(row.stableId, std::move(*thumbnail));
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

std::optional<LayersPanel::RowThumbnail> LayersPanel::rowThumbnail(std::uint64_t stableId) const {
  const auto it = thumbnailByStableId_.find(stableId);
  if (it == thumbnailByStableId_.end()) {
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

    // 24x24 preview cell. Geometry rows (path/rect/circle/…) draw a real
    // per-shape silhouette of the element's computed outline, filled/stroked
    // with its computed colors; other rows fall back to the deterministic
    // fill-derived swatch (see SwatchColorForElement / RowThumbnail for why a
    // full offscreen subtree raster is deferred).
    const ImVec2 swatchMin = ImGui::GetCursorScreenPos();
    const ImVec2 swatchMax(swatchMin.x + kPreviewSize, swatchMin.y + kPreviewSize);
    const auto thumbnailIt = thumbnailByStableId_.find(row.stableId);
    if (thumbnailIt != thumbnailByStableId_.end()) {
      const RowThumbnail& thumbnail = thumbnailIt->second;
      // Inset the geometry slightly so strokes are not clipped by the cell edge.
      constexpr float kInset = 2.0f;
      const float drawSize = kPreviewSize - kInset * 2.0f;
      drawList->AddRectFilled(swatchMin, swatchMax, IM_COL32(40, 40, 40, 255), 3.0f);
      std::vector<ImVec2> screenPoints;
      screenPoints.reserve(thumbnail.normalizedPoints.size());
      for (const Vector2d& point : thumbnail.normalizedPoints) {
        screenPoints.push_back(
            ImVec2(swatchMin.x + kInset + static_cast<float>(point.x) * drawSize,
                   swatchMin.y + kInset + static_cast<float>(point.y) * drawSize));
      }
      if (thumbnail.closed && screenPoints.size() >= 3) {
        drawList->AddConvexPolyFilled(screenPoints.data(), static_cast<int>(screenPoints.size()),
                                      ToImU32(thumbnail.fill));
      }
      drawList->AddPolyline(screenPoints.data(), static_cast<int>(screenPoints.size()),
                            ToImU32(thumbnail.stroke),
                            thumbnail.closed ? ImDrawFlags_Closed : ImDrawFlags_None, 1.5f);
      drawList->AddRect(swatchMin, swatchMax, IM_COL32(255, 255, 255, 60), 3.0f);
    } else {
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
