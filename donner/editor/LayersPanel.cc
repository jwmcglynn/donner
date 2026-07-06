#include "donner/editor/LayersPanel.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "donner/editor/EmbeddedSvgIcon.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/svg/ElementType.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/properties/PropertyRegistry.h"
#include "donner/svg/renderer/Renderer.h"
#include "embed_resources/BootstrapIcons.h"
#include "misc/cpp/imgui_stdlib.h"

namespace donner::editor {

namespace {

/// Pixel dimensions of the non-square preview cell drawn for each row.
constexpr int kPreviewWidthPx = 42;
constexpr int kPreviewHeightPx = 24;

/// Pixel size of the Bootstrap SVG icons inside the right-aligned row buttons.
constexpr float kAffordanceIconSize = 14.0f;

/// Raster size in device pixels for layer affordance icon textures.
constexpr int kAffordanceIconRasterSizePx = 32;

enum class LayerAffordanceIcon {
  Visible,
  Hidden,
  Locked,
  Unlocked,
};

std::uint64_t LayerAffordanceIconTextureKey(LayerAffordanceIcon icon) {
  constexpr std::uint64_t kIconTextureKeyBase = 0xf500000000000000ull;
  switch (icon) {
    case LayerAffordanceIcon::Visible: return kIconTextureKeyBase + 1u;
    case LayerAffordanceIcon::Hidden: return kIconTextureKeyBase + 2u;
    case LayerAffordanceIcon::Locked: return kIconTextureKeyBase + 3u;
    case LayerAffordanceIcon::Unlocked: return kIconTextureKeyBase + 4u;
  }
  return kIconTextureKeyBase;
}

std::span<const unsigned char> BootstrapSvgForIcon(LayerAffordanceIcon icon) {
  switch (icon) {
    case LayerAffordanceIcon::Visible: return embedded::kBootstrapEyeSvg;
    case LayerAffordanceIcon::Hidden: return embedded::kBootstrapEyeSlashSvg;
    case LayerAffordanceIcon::Locked: return embedded::kBootstrapLockSvg;
    case LayerAffordanceIcon::Unlocked: return embedded::kBootstrapUnlockSvg;
  }
  return embedded::kBootstrapEyeSvg;
}

const std::optional<svg::RendererBitmap>& CachedBootstrapIconBitmap(LayerAffordanceIcon icon) {
  switch (icon) {
    case LayerAffordanceIcon::Visible: {
      static const std::optional<svg::RendererBitmap> bitmap = RenderEmbeddedSvgIcon(
          BootstrapSvgForIcon(LayerAffordanceIcon::Visible), kAffordanceIconRasterSizePx);
      return bitmap;
    }
    case LayerAffordanceIcon::Hidden: {
      static const std::optional<svg::RendererBitmap> bitmap = RenderEmbeddedSvgIcon(
          BootstrapSvgForIcon(LayerAffordanceIcon::Hidden), kAffordanceIconRasterSizePx);
      return bitmap;
    }
    case LayerAffordanceIcon::Locked: {
      static const std::optional<svg::RendererBitmap> bitmap = RenderEmbeddedSvgIcon(
          BootstrapSvgForIcon(LayerAffordanceIcon::Locked), kAffordanceIconRasterSizePx);
      return bitmap;
    }
    case LayerAffordanceIcon::Unlocked: {
      static const std::optional<svg::RendererBitmap> bitmap = RenderEmbeddedSvgIcon(
          BootstrapSvgForIcon(LayerAffordanceIcon::Unlocked), kAffordanceIconRasterSizePx);
      return bitmap;
    }
  }

  static const std::optional<svg::RendererBitmap> empty;
  return empty;
}

bool DrawLayerIconButton(const char* id, LayerAffordanceIcon icon,
                         const LayersPanel::IconTextureProvider& iconTextureProvider) {
  const ImVec2 iconSize(kAffordanceIconSize, kAffordanceIconSize);
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 buttonSize(iconSize.x + style.FramePadding.x * 2.0f,
                          iconSize.y + style.FramePadding.y * 2.0f);
  const ImVec2 buttonMin = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(id, buttonSize);

  if (iconTextureProvider) {
    const std::optional<svg::RendererBitmap>& bitmap = CachedBootstrapIconBitmap(icon);
    if (bitmap.has_value()) {
      const LayersPanel::IconTexture iconTexture =
          iconTextureProvider(LayerAffordanceIconTextureKey(icon), *bitmap);
      if (iconTexture.texture != 0) {
        const ImVec2 iconMin(buttonMin.x + style.FramePadding.x,
                             buttonMin.y + style.FramePadding.y);
        const ImVec2 iconMax(iconMin.x + iconSize.x, iconMin.y + iconSize.y);
        const ImVec2 uvTopLeft(0.0f, 0.0f);
        const ImVec2 uvBottomRight(static_cast<float>(iconTexture.uvBottomRight.x),
                                   static_cast<float>(iconTexture.uvBottomRight.y));
        ImGui::GetWindowDrawList()->AddImage(iconTexture.texture, iconMin, iconMax, uvTopLeft,
                                             uvBottomRight, ImGui::GetColorU32(ImGuiCol_Text));
      }
    }
  }

  return clicked;
}

/// Draw a transparent-checkerboard background into the rect [min,max), so the
/// preview cell reads as "transparent canvas" behind a thumbnail's rendered
/// pixels (the standard alpha-checkerboard convention). Light/dark 4px squares,
/// clipped to the rounded cell by the caller's surrounding rect. This is plain
/// UI chrome (not a depiction of document geometry), so ImGui draws it.
void DrawCheckerboard(ImDrawList* drawList, const ImVec2& min, const ImVec2& max) {
  constexpr float kCell = 4.0f;
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
/// only as the fallback when no rendered thumbnail is available - e.g. a row
/// whose subtree has no boundable geometry, or a build with no GL texture
/// context. The real per-row preview is the Donner-rendered raster produced by
/// `svg::Renderer::renderElementToBitmap` in `refreshSnapshot`.
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

void LayersPanel::refreshSnapshot(const EditorApp& app, svg::Renderer* renderer) {
  model_.refresh(app);

  const auto renderStart = std::chrono::steady_clock::now();
  thumbnailRefreshStats_ = ThumbnailRefreshStats{
      .documentFrameVersion = app.document().currentFrameVersion(),
      .rowCount = model_.rows().size(),
  };

  swatchByStableId_.clear();
  if (!app.hasDocument()) {
    // No document loaded (e.g. the editor fell back to an error/empty state):
    // there are no rows to thumbnail, and `app.document().document()` below
    // would dereference an empty optional.
    thumbnailBitmapByStableId_.clear();
    return;
  }

  std::unordered_set<std::uint64_t> liveStableIds;
  liveStableIds.reserve(model_.rows().size());
  const bool renderThumbnails = !app.document().document().hasPendingRenderInvalidation();
  const Vector2i thumbnailMaxSizePx(kPreviewWidthPx, kPreviewHeightPx);
  if (!renderThumbnails) {
    thumbnailRefreshStats_.skippedForCanvasInvalidationCount = model_.rows().size();
  }
  std::optional<svg::Renderer> fallbackRenderer;
  if (renderThumbnails && renderer == nullptr) {
    fallbackRenderer.emplace();
    renderer = &*fallbackRenderer;
  }
  for (const LayerTreeRow& row : model_.rows()) {
    liveStableIds.insert(row.stableId);
    swatchByStableId_.emplace(row.stableId, SwatchColorForElement(row.element));

    if (!renderThumbnails) {
      continue;
    }

    if (const auto thumbnailIt = thumbnailBitmapByStableId_.find(row.stableId);
        thumbnailIt != thumbnailBitmapByStableId_.end() &&
        thumbnailIt->second.documentFrameVersion == thumbnailRefreshStats_.documentFrameVersion &&
        thumbnailIt->second.maxSizePx == thumbnailMaxSizePx) {
      ++thumbnailRefreshStats_.reusedCount;
      continue;
    }

    // Render the row's element subtree to a real RGBA thumbnail through the
    // Donner renderer. Rows whose subtree has no boundable geometry come back
    // empty and fall back to the swatch. The per-frame cache above avoids
    // rerasterizing thumbnails during idle sidebar refreshes.
    svg::RendererBitmap thumbnail =
        renderer->renderElementToBitmap(row.element, thumbnailMaxSizePx);
    ++thumbnailRefreshStats_.renderedCount;
    CachedThumbnail& cacheEntry = thumbnailBitmapByStableId_[row.stableId];
    cacheEntry.documentFrameVersion = thumbnailRefreshStats_.documentFrameVersion;
    cacheEntry.maxSizePx = thumbnailMaxSizePx;
    if (!thumbnail.empty()) {
      cacheEntry.bitmap = std::move(thumbnail);
    }
  }
  // Keep the previous non-empty thumbnail for a live row when an idle refresh
  // transiently misses renderable geometry; otherwise rows can flash back to
  // the fallback swatch until the next successful refresh.
  for (auto it = thumbnailBitmapByStableId_.begin(); it != thumbnailBitmapByStableId_.end();) {
    if (liveStableIds.count(it->first) == 0) {
      it = thumbnailBitmapByStableId_.erase(it);
    } else {
      ++it;
    }
  }

  // Drop a stale active row if it no longer maps to a visible row.
  if (activeRowIndex_.has_value() && *activeRowIndex_ >= model_.rows().size()) {
    activeRowIndex_.reset();
  }
  if (anchorRowIndex_.has_value() && *anchorRowIndex_ >= model_.rows().size()) {
    anchorRowIndex_.reset();
  }
  const auto renderEnd = std::chrono::steady_clock::now();
  thumbnailRefreshStats_.renderMs =
      std::chrono::duration<double, std::milli>(renderEnd - renderStart).count();
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
  if (it == thumbnailBitmapByStableId_.end() || it->second.bitmap.empty()) {
    return nullptr;
  }
  return &it->second.bitmap;
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

void LayersPanel::setLockedRejectionFlash(std::optional<LayersLockedRejectionFlash> flash) {
  lockedRejectionFlash_ = std::move(flash);
}

std::optional<std::size_t> LayersPanel::flashedRowIndex() const {
  if (!lockedRejectionFlash_.has_value() || lockedRejectionFlash_->intensity <= 0.0f) {
    return std::nullopt;
  }
  const std::vector<LayerTreeRow>& rows = model_.rows();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].element == lockedRejectionFlash_->element) {
      return i;
    }
  }
  return std::nullopt;
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
  // shell flushes the queued mutation and re-renders - otherwise the canvas
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

void LayersPanel::render(EditorApp* liveApp, const ThumbnailTextureProvider& textureProvider,
                         const IconTextureProvider& iconTextureProvider) {
  const std::vector<LayerTreeRow>& rows = model_.rows();
  if (rows.empty()) {
    ImGui::TextDisabled("(no document)");
    return;
  }

  constexpr float kPreviewWidth = static_cast<float>(kPreviewWidthPx);
  constexpr float kPreviewHeight = static_cast<float>(kPreviewHeightPx);
  constexpr float kIndentStep = 14.0f;
  ImDrawList* drawList = ImGui::GetWindowDrawList();

  // The visible row (if any) whose element matches the active locked-rejection
  // flash. Its background flashes red in sync with the canvas outline flash; the
  // alpha tracks the flash fade intensity.
  const std::optional<std::size_t> flashRowIndex = flashedRowIndex();

  std::optional<std::size_t> hoveredRowIndex;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const LayerTreeRow& row = rows[i];
    ImGui::PushID(static_cast<int>(static_cast<std::uint32_t>(row.stableId)));

    // For the flashed row, capture the row's top-left in screen space and split
    // the draw list so the red highlight rect lands *behind* the row content
    // (chevron, preview, label, affordances). The rect is filled after the row
    // is laid out, when its full height is known. This is plain UI chrome (a row
    // background), not a depiction of vector artwork - see CLAUDE.md "No
    // Rendering Vector Graphics With ImGui".
    const bool isFlashRow = flashRowIndex.has_value() && *flashRowIndex == i;
    const ImVec2 rowFlashTopLeft = ImGui::GetCursorScreenPos();
    if (isFlashRow) {
      drawList->ChannelsSplit(2);
      drawList->ChannelsSetCurrent(1);
    }

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

    // Preview cell. The cell shows the row's Donner-rendered thumbnail
    // (the element subtree rasterized in refreshSnapshot, uploaded to an ImGui
    // texture by the shell-provided textureProvider and blitted via
    // ImGui::Image) over a transparent-canvas checkerboard. ImGui never draws
    // the vector geometry itself - only the Donner-produced texture. Rows with
    // no rendered thumbnail (or no GL texture context) fall back to the
    // deterministic fill swatch. See CLAUDE.md "No Rendering Vector Graphics
    // With ImGui".
    const ImVec2 slotMin = ImGui::GetCursorScreenPos();
    const ImVec2 slotMax(slotMin.x + kPreviewWidth, slotMin.y + kPreviewHeight);
    const svg::RendererBitmap* thumbnailBitmap = nullptr;
    ThumbnailTexture thumbnailTexture;
    if (textureProvider) {
      if (const auto thumbnailIt = thumbnailBitmapByStableId_.find(row.stableId);
          thumbnailIt != thumbnailBitmapByStableId_.end() && !thumbnailIt->second.bitmap.empty()) {
        thumbnailBitmap = &thumbnailIt->second.bitmap;
        thumbnailTexture = textureProvider(row.stableId, *thumbnailBitmap);
      }
    }

    ImVec2 thumbnailSize(kPreviewHeight, kPreviewHeight);
    if (thumbnailTexture.texture != 0 && thumbnailBitmap != nullptr) {
      thumbnailSize =
          ImVec2(std::min(kPreviewWidth, static_cast<float>(thumbnailBitmap->dimensions.x)),
                 std::min(kPreviewHeight, static_cast<float>(thumbnailBitmap->dimensions.y)));
    }

    const ImVec2 swatchMin(slotMin.x + (kPreviewWidth - thumbnailSize.x) * 0.5f,
                           slotMin.y + (kPreviewHeight - thumbnailSize.y) * 0.5f);
    const ImVec2 swatchMax(swatchMin.x + thumbnailSize.x, swatchMin.y + thumbnailSize.y);
    // Transparent-canvas checkerboard behind every rendered thumbnail, clipped
    // to the variable-sized preview rect so the visible cell follows the
    // element content aspect ratio.
    drawList->PushClipRect(swatchMin, swatchMax, /*intersect_with_current=*/true);
    DrawCheckerboard(drawList, swatchMin, swatchMax);
    drawList->PopClipRect();

    if (thumbnailTexture.texture != 0) {
      // Blit the Donner-rendered thumbnail texture over the checkerboard.
      const ImVec2 uvTopLeft(0.0f, 0.0f);
      const ImVec2 uvBottomRight(static_cast<float>(thumbnailTexture.uvBottomRight.x),
                                 static_cast<float>(thumbnailTexture.uvBottomRight.y));
      drawList->AddImage(thumbnailTexture.texture, swatchMin, swatchMax, uvTopLeft, uvBottomRight);
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
    ImGui::Dummy(ImVec2(slotMax.x - slotMin.x, slotMax.y - slotMin.y));
    ImGui::SameLine();

    // Selection highlight. Only a *truly* selected row gets the filled row
    // highlight. A group that merely contains the selection (partial) gets a
    // subtle left-edge accent bar instead (drawn after the row), so selecting a
    // nested shape no longer paints every ancestor group - down to the
    // top-level `<g>[0]` - as if it were selected. That ancestor-fill was the
    // "selection defaults to the top <g>[0] at all times" report.
    const bool partialOnly = row.isPartiallySelected && !row.isSelected;
    const bool selected = row.isSelected;
    // Subtle hover/active tint for the row: the stock ImGui header-hover fill is
    // too bright for a dense layer list, so scale its alpha well down.
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                          ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.28f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImGui::GetColorU32(ImGuiCol_HeaderActive, 0.45f));
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
      // full-width Selectable) actually receive clicks - without it the
      // SpanAllColumns Selectable claims the whole row's hit area and the
      // buttons are dead.
      const ImVec2 labelMin = ImGui::GetCursorScreenPos();
      if (ImGui::Selectable("##layer_row_select", selected,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                            ImVec2(0.0f, kPreviewHeight))) {
        if (liveApp != nullptr) {
          ClickModifiers mods{
              .shift = ImGui::GetIO().KeyShift,
              .ctrl = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper,
          };
          handleRowClick(*liveApp, i, mods);
        }
      }
      const ImVec2 labelPos(labelMin.x,
                            slotMin.y + (kPreviewHeight - ImGui::GetTextLineHeight()) * 0.5f);
      drawList->AddText(labelPos, ImGui::GetColorU32(ImGuiCol_Text), row.displayName.c_str());
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
    ImGui::PopStyleColor(2);
    // Subtle partial-selection affordance: a thin accent bar at the row's left
    // edge marks a group that contains the selection, without the loud
    // full-row highlight a real selection gets.
    if (partialOnly) {
      const ImU32 accent = ImGui::GetColorU32(ImGuiCol_Header, 0.9f);
      const ImVec2 barMin(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x, slotMin.y);
      const ImVec2 barMax(barMin.x + 2.0f, slotMin.y + kPreviewHeight);
      drawList->AddRectFilled(barMin, barMax, accent, 1.0f);
    }

    // Right-aligned per-row affordances: a visibility (eye) toggle and a lock
    // toggle. Drawn after the Selectable so they paint on top and capture
    // clicks before the row's selection hit area. Both funnel through the
    // shared handleEyeClick / handleLockClick seams (the same path the context
    // menu uses) and are dropped when the worker owns the document (liveApp ==
    // nullptr), mirroring the selection-click guard.
    const LayerAffordanceIcon eyeIcon =
        row.isVisible ? LayerAffordanceIcon::Visible : LayerAffordanceIcon::Hidden;
    const LayerAffordanceIcon lockIcon =
        row.isLocked ? LayerAffordanceIcon::Locked : LayerAffordanceIcon::Unlocked;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttonWidth = kAffordanceIconSize + style.FramePadding.x * 2.0f;
    const float affordancesWidth = buttonWidth * 2.0f + style.ItemSpacing.x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - affordancesWidth);
    if (DrawLayerIconButton("##layer_eye", eyeIcon, iconTextureProvider) && liveApp != nullptr) {
      handleEyeClick(*liveApp, i);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", row.isVisible ? "Hide layer" : "Show layer");
    }
    ImGui::SameLine();
    if (DrawLayerIconButton("##layer_lock", lockIcon, iconTextureProvider) && liveApp != nullptr) {
      handleLockClick(*liveApp, i);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", row.isLocked ? "Unlock layer" : "Lock layer");
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
      // Arrange (paint/z-order) - routes through the shared reorderSelectedElement
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
      // TODO(v0.8): Delete from the Layers panel - needs the in-sync source
      // text that lives in EditorShell, so it is wired at the shell level
      // rather than inventing a new EditorApp API here.
      ImGui::MenuItem("Delete", nullptr, false, false);
      ImGui::EndPopup();
    }

    // Paint the locked-rejection flash behind the flashed row, then merge the
    // split channels so the red background sits under the row content. The red
    // matches the canvas outline flash color family (RGBA 0xFF,0x1A,0x1A); its
    // alpha scales with the flash fade intensity (1 -> 0).
    if (isFlashRow) {
      drawList->ChannelsSetCurrent(0);
      const float flashIntensity =
          std::clamp(lockedRejectionFlash_ ? lockedRejectionFlash_->intensity : 0.0f, 0.0f, 1.0f);
      const ImU32 flashColor =
          IM_COL32(0xff, 0x1a, 0x1a, static_cast<int>(flashIntensity * 160.0f));
      const float rowBottomY = ImGui::GetCursorScreenPos().y;
      const ImVec2 flashMin(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x,
                            rowFlashTopLeft.y);
      const ImVec2 flashMax(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x,
                            rowBottomY);
      drawList->AddRectFilled(flashMin, flashMax, flashColor, 3.0f);
      drawList->ChannelsMerge();
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
