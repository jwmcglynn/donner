/// @file
/// EditorControlSession render tools and diagnostics: driving the async
/// renderer for captured frames, the headless texture cache that mirrors the
/// editor's presented-frame composition, and compositor/session state JSON.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/PresentationRenderScheduler.h"
#include "donner/editor/PresentedFrameComposer.h"
#include "donner/editor/ViewportState.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/IdComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/compositor/ScopedCompositorHint.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "nlohmann/json.hpp"
#include "tools/mcp-servers/editor-control/EditorControlSessionInternal.h"

namespace donner::editor::mcp {

namespace {

using nlohmann::json;

std::string EntityDebugLabel(const Registry& registry, Entity entity) {
  if (!registry.valid(entity)) {
    return "null";
  }

  std::string label;
  if (const auto* tree = registry.try_get<donner::components::TreeComponent>(entity)) {
    label = std::string(tree->tagName().name);
  } else {
    label = "entity";
  }
  if (const auto* id = registry.try_get<svg::components::IdComponent>(entity);
      id != nullptr && !id->id().empty()) {
    label.push_back('#');
    label.append(std::string_view(id->id()));
  }
  label.push_back(' ');
  label.push_back('#');
  label.append(std::to_string(EntityToJsonValue(entity)));
  return label;
}

std::optional<SelectTool::ActiveDragPreview> DragPreviewFromRenderRequest(
    const std::optional<RenderRequest::DragPreview>& preview) {
  if (!preview.has_value() || preview->entity == entt::null) {
    return std::nullopt;
  }

  return SelectTool::ActiveDragPreview{
      .entity = preview->entity,
      .extraEntities = preview->extraEntities,
      .translation = preview->translation,
      .documentFromCachedDocument = preview->documentFromCachedDocument,
      .dragGeneration = preview->dragGeneration,
  };
}

json DirtyFlagsToJson(std::uint16_t flags) {
  using DirtyFlags = svg::components::DirtyFlagsComponent;
  const std::array<std::pair<DirtyFlags::Flags, std::string_view>, 10> names{{
      {DirtyFlags::Style, "style"},
      {DirtyFlags::Layout, "layout"},
      {DirtyFlags::Transform, "transform"},
      {DirtyFlags::WorldTransform, "world_transform"},
      {DirtyFlags::Shape, "shape"},
      {DirtyFlags::Paint, "paint"},
      {DirtyFlags::Filter, "filter"},
      {DirtyFlags::RenderInstance, "render_instance"},
      {DirtyFlags::ShadowTree, "shadow_tree"},
      {DirtyFlags::TextGeometry, "text_geometry"},
  }};

  json out = json::array();
  for (const auto& [flag, name] : names) {
    if ((flags & flag) != 0) {
      out.push_back(name);
    }
  }
  return out;
}

std::string CompositeTileKindName(
    svg::compositor::CompositorController::CompositeTileSnapshot::Kind kind) {
  using Kind = svg::compositor::CompositorController::CompositeTileSnapshot::Kind;
  switch (kind) {
    case Kind::Background: return "background";
    case Kind::Foreground: return "foreground";
    case Kind::Segment: return "segment";
    case Kind::Layer: return "layer";
  }
  return "unknown";
}

std::string PromoteRefusalReasonName(
    svg::compositor::CompositorController::PromoteRefusalReason reason) {
  using Reason = svg::compositor::CompositorController::PromoteRefusalReason;
  switch (reason) {
    case Reason::None: return "none";
    case Reason::InvalidEntity: return "invalid_entity";
    case Reason::LayerLimit: return "layer_limit";
    case Reason::MemoryLimit: return "memory_limit";
    case Reason::DescendantPromoted: return "descendant_promoted";
  }
  return "unknown";
}

json CompositeTileSnapshotJson(
    const svg::compositor::CompositorController::CompositeTileSnapshot& tile, int index) {
  return json{
      {"index", index},
      {"kind", CompositeTileKindName(tile.kind)},
      {"id", tile.id},
      {"label", tile.label},
      {"bitmap_dims", VectorToJson(tile.bitmapDims)},
      {"generation", tile.generation},
      {"last_rasterize_ms", tile.lastRasterizeMs},
      {"has_valid_bitmap", tile.hasValidBitmap},
      {"is_drag_target", tile.isDragTarget},
      {"span_range_label", tile.spanRangeLabel},
      {"immediate", tile.immediate},
      {"visible", tile.visible},
      {"thumbnail_dims", VectorToJson(tile.thumbnailDims)},
      {"thumbnail_bytes", tile.thumbnailPixels.size()},
  };
}

}  // namespace

void EditorControlSession::HeadlessTextureCache::reset() {
  tileTextures_.clear();
  tiles_.clear();
}

void EditorControlSession::HeadlessTextureCache::uploadComposited(
    const RenderResult::CompositedPreview& preview) {
  std::unordered_set<std::string> liveIds;
  liveIds.reserve(preview.tiles.size());

  tiles_.clear();
  tiles_.reserve(preview.tiles.size());

  for (const RenderResult::CompositedTile& tile : preview.tiles) {
    svg::RendererBitmap textureReadback;
    const svg::RendererBitmap* payload = &tile.bitmap;
    if (payload->empty() && tile.textureSnapshot != nullptr) {
      textureReadback = tile.textureSnapshot->takeSnapshot();
      payload = &textureReadback;
    }

    const bool reusingExistingTexture = payload->empty();
    if (reusingExistingTexture) {
      const auto cachedIt = tileTextures_.find(tile.id);
      if (cachedIt == tileTextures_.end()) {
        continue;
      }

      liveIds.insert(tile.id);
      DisplayTileView view;
      view.kind = cachedIt->second.kind;
      view.id = tile.id;
      view.generation = cachedIt->second.generation;
      view.bitmapDimsPx = cachedIt->second.bitmapDimsPx;
      view.canvasOffsetDoc = tile.canvasOffsetDoc;
      view.bitmapDimsDoc = tile.bitmapDimsDoc;
      view.dragTranslationDoc = tile.dragTranslationDoc;
      view.documentFromCachedDocument = tile.documentFromCachedDocument;
      view.isDragTarget = tile.isDragTarget;
      view.reusedPreviousTexture = true;
      view.contentHash = cachedIt->second.contentHash;
      tiles_.push_back(std::move(view));
      continue;
    }

    liveIds.insert(tile.id);

    CachedTextureEntry& entry = tileTextures_[tile.id];
    const bool needsUpload = entry.bitmap.empty() || entry.generation != tile.generation ||
                             entry.bitmapDimsPx != payload->dimensions;
    if (needsUpload) {
      entry.generation = tile.generation;
      entry.bitmapDimsPx = payload->dimensions;
      entry.contentHash = BitmapContentHash(*payload).value_or("");
      entry.bitmap = *payload;
    }
    entry.kind = tile.kind;

    DisplayTileView view;
    view.kind = tile.kind;
    view.id = tile.id;
    view.generation = tile.generation;
    view.bitmapDimsPx = payload->dimensions;
    view.canvasOffsetDoc = tile.canvasOffsetDoc;
    view.bitmapDimsDoc = tile.bitmapDimsDoc;
    view.dragTranslationDoc = tile.dragTranslationDoc;
    view.documentFromCachedDocument = tile.documentFromCachedDocument;
    view.isDragTarget = tile.isDragTarget;
    view.reusedPreviousTexture = !needsUpload;
    view.contentHash = entry.contentHash;
    tiles_.push_back(std::move(view));
  }

  for (auto it = tileTextures_.begin(); it != tileTextures_.end();) {
    if (liveIds.find(it->first) == liveIds.end()) {
      it = tileTextures_.erase(it);
    } else {
      ++it;
    }
  }
}

std::optional<svg::RendererBitmap> EditorControlSession::HeadlessTextureCache::composeDisplayFrame(
    const DisplayFrameSnapshot& display, const Box2d& viewBox, const Vector2i& canvasSize) const {
  if (display.path != "tiles" || canvasSize.x <= 0 || canvasSize.y <= 0 ||
      viewBox.size().x <= 0.0 || viewBox.size().y <= 0.0) {
    return std::nullopt;
  }

  svg::Renderer renderer;
  renderer.beginFrame(svg::RenderViewport{
      .size = Vector2d(static_cast<double>(canvasSize.x), static_cast<double>(canvasSize.y)),
      .devicePixelRatio = 1.0,
  });

  const double pixelsPerDocX = static_cast<double>(canvasSize.x) / viewBox.size().x;
  const double pixelsPerDocY = static_cast<double>(canvasSize.y) / viewBox.size().y;
  const Transform2d canvasPixelsFromCanvasTransform =
      Transform2d::Translate(-viewBox.topLeft) *
      Transform2d::Scale(Vector2d(pixelsPerDocX, pixelsPerDocY));
  const std::optional<PresentedDragBaseline> dragBaseline =
      PresentedBaselineFromSelectPreviews(display.activeDragPreview, display.displayedDragPreview);
  for (const DisplayTileView& tile : display.tiles) {
    const auto tileIt = tileTextures_.find(tile.id);
    if (tileIt == tileTextures_.end() || tileIt->second.bitmap.empty()) {
      continue;
    }

    const std::optional<PresentedTileQuad> tileQuad = ComputePresentedTileQuad(
        PresentedGeometryFromDisplayTile(tile), canvasPixelsFromCanvasTransform, dragBaseline);
    if (!tileQuad.has_value()) {
      continue;
    }
    const svg::RendererBitmap& bitmap = tileIt->second.bitmap;
    const std::optional<Transform2d> canvasPixelsFromTexture =
        ComputePresentedOutputFromTextureTransform(*tileQuad, bitmap.dimensions);
    if (!canvasPixelsFromTexture.has_value()) {
      continue;
    }

    renderer.setTransform(*canvasPixelsFromTexture);
    renderer.drawBitmap(
        bitmap, svg::ImageParams{
                    .targetRect =
                        Box2d(Vector2d::Zero(), Vector2d(static_cast<double>(bitmap.dimensions.x),
                                                         static_cast<double>(bitmap.dimensions.y))),
                });
  }

  renderer.endFrame();
  return renderer.takeSnapshot();
}

ToolCallResult EditorControlSession::renderFrameTool(const json& arguments) {
  std::string error;
  if (!ensureDocumentLoaded(&error)) {
    return MakeErrorResult(error);
  }
  (void)drainPendingWritebacks();

  ToolCallResult out;
  CaptureOptions capture;
  if (!ReadCaptureOptions(arguments, true, &capture, &error)) {
    return MakeErrorResult(error);
  }
  std::vector<CapturedRenderResult> renderResults;
  if (!renderCurrentFrame(&renderResults, &error)) {
    return MakeErrorResult(error);
  }
  out.body = json{
      {"ok", true},
      {"source", sourceStateJson()},
      {"selection", selectedElementJson()},
      {"stages", RenderResultsJson(renderResults, &out, capture, "render_frame")},
  };
  out.body["attached_image_count"] = out.images.size();
  return out;
}

ToolCallResult EditorControlSession::sessionState(const json&) const {
  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"has_document", app_.hasDocument()},
      {"canvas", {{"width", canvasWidth_}, {"height", canvasHeight_}}},
      {"device_pixel_ratio", devicePixelRatio_},
      {"source", sourceStateJson()},
      {"selection", selectedElementJson()},
      {"worker_compositor_entity", EntityToJsonValue(asyncRenderer_.workerCompositorEntity())},
  };

  const auto state = asyncRenderer_.compositorState();
  out.body["compositor_state"] = json{
      {"active_hints_count", state.activeHintsCount},
      {"layer_count", state.layerCount},
      {"split_path_active", state.splitPathActive},
      {"split_static_layers_entity", EntityToJsonValue(state.splitStaticLayersEntity)},
      {"canvas_size", VectorToJson(state.canvasSize)},
      {"last_promote_refusal_reason", PromoteRefusalReasonName(state.lastPromoteRefusalReason)},
      {"last_promote_refusal_entity", EntityToJsonValue(state.lastPromoteRefusalEntity)},
  };

  if (app_.hasDocument()) {
    const svg::SVGDocument& document = app_.document().document();
    [[maybe_unused]] svg::DocumentReadAccess access = document.readAccess();
    const Registry& registry = document.registry();
    json dirtyEntities = json::array();
    for (const Entity entity : registry.view<svg::components::DirtyFlagsComponent>()) {
      const auto& dirty = registry.get<svg::components::DirtyFlagsComponent>(entity);
      dirtyEntities.push_back(json{
          {"entity", EntityToJsonValue(entity)},
          {"flags", dirty.flags},
          {"names", DirtyFlagsToJson(dirty.flags)},
      });
    }

    json renderTreeState = nullptr;
    if (const auto* stateComponent = registry.ctx().find<svg::components::RenderTreeState>()) {
      renderTreeState = json{
          {"has_been_built", stateComponent->hasBeenBuilt},
          {"needs_full_rebuild", stateComponent->needsFullRebuild},
          {"needs_full_style_recompute", stateComponent->needsFullStyleRecompute},
      };
    }
    out.body["render_tree"] = json{
        {"dirty_count", dirtyEntities.size()},
        {"dirty_entities", std::move(dirtyEntities)},
        {"state", std::move(renderTreeState)},
    };

    json renderInstances = json::array();
    for (auto view = registry.view<const svg::components::RenderingInstanceComponent>();
         const Entity entity : view) {
      const auto& instance = view.get<const svg::components::RenderingInstanceComponent>(entity);
      json styleJson = nullptr;
      if (const auto* style = registry.try_get<svg::components::ComputedStyleComponent>(entity);
          style != nullptr && style->properties.has_value()) {
        styleJson = json{
            {"display",
             style->properties->display.get().value() == svg::Display::None ? "none" : "other"},
            {"visibility", static_cast<int>(style->properties->visibility.get().value())},
        };
      }
      renderInstances.push_back(json{
          {"entity", EntityToJsonValue(entity)},
          {"label", EntityDebugLabel(registry, entity)},
          {"data_entity", EntityToJsonValue(instance.dataEntity)},
          {"data_label", EntityDebugLabel(registry, instance.dataEntity)},
          {"draw_order", instance.drawOrder},
          {"visible", instance.visible},
          {"style", std::move(styleJson)},
      });
    }
    out.body["render_instances"] = std::move(renderInstances);
  } else {
    out.body["render_tree"] = nullptr;
    out.body["render_instances"] = json::array();
  }

  json compositeTiles = json::array();
  int index = 0;
  for (const auto& tile : asyncRenderer_.compositorCompositeTiles()) {
    compositeTiles.push_back(CompositeTileSnapshotJson(tile, index++));
  }
  out.body["composite_tiles"] = std::move(compositeTiles);
  return out;
}

bool EditorControlSession::drainPendingWritebacks() {
  std::vector<EditorApp::CompletedTransformWriteback> transformWritebacks;
  if (auto completed = selectTool_->consumeCompletedDragWriteback(); completed.has_value()) {
    transformWritebacks.push_back(EditorApp::CompletedTransformWriteback{
        .target = std::move(completed->target),
        .transform = completed->transform,
    });
    for (auto& extra : completed->extras) {
      transformWritebacks.push_back(EditorApp::CompletedTransformWriteback{
          .target = std::move(extra.target),
          .transform = extra.transform,
      });
    }
  }
  if (auto completed = app_.consumeTransformWriteback(); completed.has_value()) {
    transformWritebacks.push_back(std::move(*completed));
  }

  bool changed = false;
  if (app_.hasDocument() && app_.document().document().hasSourceStore()) {
    svg::SVGDocument& document = app_.document().document();
    for (const EditorApp::CompletedTransformWriteback& writeback : transformWritebacks) {
      std::optional<svg::SVGElement> element =
          resolveAttributeWritebackTarget(document, writeback.target);
      if (!element.has_value()) {
        continue;
      }

      if (writeback.restoreSourceTransformAttributeValue) {
        if (writeback.sourceTransformAttributeValue.has_value()) {
          (void)document.setElementAttribute(*element, "transform",
                                             *writeback.sourceTransformAttributeValue);
        } else {
          (void)document.removeElementAttribute(*element, "transform");
        }
      } else {
        const RcString serialized = toSVGTransformString(writeback.transform);
        if (std::string_view(serialized).empty()) {
          (void)document.removeElementAttribute(*element, "transform");
        } else {
          (void)document.setElementAttribute(*element, "transform", serialized);
        }
      }
      changed = true;
    }
  }

  const std::vector<EditorApp::CompletedElementRemoveWriteback> elementRemoveWritebacks =
      app_.consumeElementRemoveWritebacks();
  changed = changed || !elementRemoveWritebacks.empty();
  if (changed && app_.hasDocument()) {
    syncSourceTextFromDocumentIfChanged();
  }
  return changed;
}

bool EditorControlSession::renderCurrentFrame(std::vector<CapturedRenderResult>* results,
                                              std::string* error) {
  results->clear();
  if (!ensureDocumentLoaded(error)) {
    return false;
  }

  const std::optional<SelectTool::ActiveDragPreview> activePreview =
      selectTool_->activeDragPreview();
  const Entity selectedEntity = SelectedGraphicsEntity(app_);
  displayPresentation_.clearSettlingIfSelectionChanged(selectedEntity, activePreview.has_value());

  RenderRequest request(renderer_, app_.document().document());
  request.captureCpuSnapshot = true;
  request.version = nextRenderVersion_++;
  request.documentGeneration = app_.document().documentGeneration();
  request.structuralRemap = app_.document().consumePendingStructuralRemap();

  if (app_.selectedElement().has_value()) {
    request.selection = *app_.selectedElement();
    request.selectedEntity = selectedEntity;
  }

  if (activePreview.has_value()) {
    request.dragPreview = RenderRequest::DragPreview{
        .entity = activePreview->entity,
        .extraEntities = activePreview->extraEntities,
        .interactionKind = svg::compositor::InteractionHint::ActiveDrag,
        .translation = activePreview->translation,
        .documentFromCachedDocument = activePreview->documentFromCachedDocument,
        .dragGeneration = activePreview->dragGeneration,
    };
  } else if (request.selectedEntity != entt::null) {
    request.dragPreview = RenderRequest::DragPreview{
        .entity = request.selectedEntity,
        .interactionKind = svg::compositor::InteractionHint::Selection,
    };
  }

  asyncRenderer_.requestRender(request);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto result = asyncRenderer_.pollResult(); result.has_value()) {
      DisplayFrameSnapshot displayFrame = recordDisplayFrame(*result);
      std::optional<svg::RendererBitmap> presentedBitmap = composeDisplayFrameBitmap(displayFrame);
      results->push_back(CapturedRenderResult{
          .renderResult = std::move(*result),
          .displayFrame = std::move(displayFrame),
          .presentedBitmap = std::move(presentedBitmap),
      });
      // A completed render can leave a deferred first-frame cache warmup queued on the
      // worker. That warmup traverses the document from the worker thread, while the next
      // tool call mutates the DOM from this thread through `EditorApp::flushFrame()`, which
      // applies queued commands without taking document write access. The interactive shell
      // absorbs the deferred work in its idle frame loop and gates every flush on
      // `!isBusy()`; this session has no such loop, so a tool call must not return while
      // speculative worker work is still walking the registry.
      const auto warmupDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      while (asyncRenderer_.isBusy() && std::chrono::steady_clock::now() < warmupDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (asyncRenderer_.isBusy()) {
        *error = "timed out waiting for the deferred compositor warmup to settle";
        return false;
      }
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  *error = "timed out waiting for async render result";
  return false;
}

std::optional<svg::RendererBitmap> EditorControlSession::composeDisplayFrameBitmap(
    const DisplayFrameSnapshot& display) const {
  const Box2d fallback = Box2d::FromXYWH(0.0, 0.0, canvasWidth_, canvasHeight_);
  const Box2d viewBox = DocumentViewBoxOr(app_.document().document(), fallback);
  return displayTextures_.composeDisplayFrame(display, viewBox,
                                              Vector2i(canvasWidth_, canvasHeight_));
}

EditorControlSession::DisplayFrameSnapshot EditorControlSession::recordDisplayFrame(
    const RenderResult& result) {
  if (result.compositedPreview.has_value() && result.compositedPreview->valid()) {
    displayTextures_.uploadComposited(*result.compositedPreview);
    displayPresentation_.noteCachedTextures(
        result.compositedPreview->entity, result.version, app_.document().document().canvasSize(),
        DragPreviewFromRenderRequest(result.compositedPreview->representedDragPreview));
  }

  return currentDisplayFrame();
}

EditorControlSession::DisplayFrameSnapshot EditorControlSession::currentDisplayFrame() const {
  const std::optional<SelectTool::ActiveDragPreview> activePreview =
      selectTool_->activeDragPreview();
  const std::optional<SelectTool::ActiveDragPreview> displayedPreview =
      displayPresentation_.presentationPreview(activePreview);
  const CompositedPresentation::DiagnosticsSnapshot presentation =
      displayPresentation_.diagnostics();

  DisplayFrameSnapshot frame;
  frame.hasCachedTiles = presentation.hasCachedTextures;
  frame.cachedEntity = presentation.cachedEntity;
  frame.hasActiveDragPreview = activePreview.has_value();
  frame.activeDragPreview = activePreview;
  frame.displayedDragPreview = displayedPreview;
  frame.displayedEntity = displayedPreview.has_value() ? displayedPreview->entity : entt::null;
  if (!displayTextures_.tiles().empty()) {
    frame.path = "tiles";
    frame.tiles = displayTextures_.tiles();
  } else {
    frame.path = "empty";
  }

  return frame;
}

}  // namespace donner::editor::mcp
