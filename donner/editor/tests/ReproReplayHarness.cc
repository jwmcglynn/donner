#include "donner/editor/tests/ReproReplayHarness.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>

#include "donner/base/Vector2.h"
#include "donner/editor/RenderCoordinator.h"
#include "donner/editor/SelectionAabb.h"
#include "donner/editor/ViewportState.h"
#include "donner/editor/backend_lib/EditorApp.h"
#include "donner/editor/backend_lib/SelectTool.h"
#include "donner/editor/repro/ReproFile.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/compositor/ScopedCompositorHint.h"
#include "donner/svg/renderer/Renderer.h"
#include "gtest/gtest.h"

namespace donner::editor {

namespace {

std::string LoadFileOrEmpty(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return {};
  std::ostringstream buf;
  buf << file.rdbuf();
  return buf.str();
}

// Drain one async render: request, poll until result lands, return
// the full RenderResult. Times out after a generous deadline so a
// stuck worker is caught with a clear failure.
std::optional<RenderResult> DrainOneRenderFull(AsyncRenderer& asyncRenderer,
                                               svg::Renderer& renderer,
                                               svg::SVGDocument& document, EditorApp& editorApp,
                                               SelectTool& selectTool, std::uint64_t version) {
  RenderRequest request;
  request.renderer = &renderer;
  request.document = &document;
  request.version = version;
  request.documentGeneration = editorApp.document().documentGeneration();
  request.structuralRemap = editorApp.document().consumePendingStructuralRemap();
  if (editorApp.selectedElement().has_value() &&
      editorApp.selectedElement()->isa<svg::SVGGraphicsElement>()) {
    request.selectedEntity = editorApp.selectedElement()->entityHandle().entity();
  }
  if (auto preview = selectTool.activeDragPreview(); preview.has_value()) {
    request.dragPreview = RenderRequest::DragPreview{
        .entity = preview->entity,
        .interactionKind = svg::compositor::InteractionHint::ActiveDrag,
    };
  } else if (request.selectedEntity != entt::null) {
    request.dragPreview = RenderRequest::DragPreview{
        .entity = request.selectedEntity,
        .interactionKind = svg::compositor::InteractionHint::Selection,
    };
  }
  asyncRenderer.requestRender(request);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    auto result = asyncRenderer.pollResult();
    if (result.has_value()) {
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return std::nullopt;
}

}  // namespace

ReplayResults ReplayRepro(const std::filesystem::path& reproPath,
                          const std::filesystem::path& svgPath, const ReplayConfig& config) {
  ReplayResults results;

  const std::string svgSource = LoadFileOrEmpty(svgPath);
  if (svgSource.empty()) {
    ADD_FAILURE() << "svg source missing: " << svgPath;
    return results;
  }
  auto reproOpt = repro::ReadReproFile(reproPath);
  if (!reproOpt.has_value()) {
    ADD_FAILURE() << "repro parse failed: " << reproPath;
    return results;
  }
  const repro::ReproFile& repro = *reproOpt;

  // v2 carries per-frame `mdx`/`mdy` and a viewport snapshot. A
  // recording that lacks both is almost certainly pre-instrumentation
  // data — replay from those is the exact brokenness v2 was built to
  // eliminate. Fail loudly rather than silently produce bogus clicks.
  //
  // The reader back-propagates the first known viewport to any
  // leading pre-layout frames, so checking `front()` here is the
  // right query: if the file has any viewport at all, every frame
  // carries one by this point.
  const bool hasViewport =
      !repro.frames.empty() && repro.frames.front().viewport.has_value();
  const bool hasDocCoords = std::any_of(repro.frames.begin(), repro.frames.end(),
                                        [](const repro::ReproFrame& f) {
                                          return f.mouseDocX.has_value() && f.mouseDocY.has_value();
                                        });
  if (!hasViewport || !hasDocCoords) {
    ADD_FAILURE() << "repro " << reproPath << " is missing v2 viewport / doc-coord payload. "
                  << "Rerecord with the current editor binary "
                  << "(`bazel run //donner/editor:editor -- --save-repro <path> <svg>`).";
    return results;
  }

  EditorApp app;
  if (!app.loadFromString(svgSource)) {
    ADD_FAILURE() << "loadFromString failed";
    return results;
  }

  SelectTool selectTool;
  selectTool.setCompositedDragPreviewEnabled(config.forceCompositedDragPreview ||
                                             repro.metadata.experimentalMode);

  RenderCoordinator renderCoordinator;

  // Seed the viewport from the recording's first `vp` block. This is
  // the whole point of the v2 format — no pane-layout reconstruction.
  ViewportState viewport;
  {
    const auto& vp = *repro.frames.front().viewport;
    viewport.paneOrigin = Vector2d(vp.paneOriginX, vp.paneOriginY);
    viewport.paneSize = Vector2d(vp.paneSizeW, vp.paneSizeH);
    viewport.devicePixelRatio = vp.devicePixelRatio;
    viewport.zoom = vp.zoom;
    viewport.panDocPoint = Vector2d(vp.panDocX, vp.panDocY);
    viewport.panScreenPoint = Vector2d(vp.panScreenX, vp.panScreenY);
    viewport.documentViewBox =
        Box2d::FromXYWH(vp.viewBoxX, vp.viewBoxY, vp.viewBoxW, vp.viewBoxH);
  }

  app.document().document().setCanvasSize(viewport.desiredCanvasSize().x,
                                          viewport.desiredCanvasSize().y);

  // De-dup + fast-lookup set of checkpoint frames.
  const std::set<std::uint64_t> checkpointSet(config.checkpointFrames.begin(),
                                              config.checkpointFrames.end());
  std::size_t nextCheckpointIdx = 0;

  bool leftHeld = false;
  std::uint64_t version = 1;

  for (const auto& frame : repro.frames) {
    ReplayFrameTiming timing;
    timing.reproFrameIndex = frame.index;
    const auto frameStart = std::chrono::steady_clock::now();

    if (frame.viewport.has_value()) {
      const auto& vp = *frame.viewport;
      viewport.paneOrigin = Vector2d(vp.paneOriginX, vp.paneOriginY);
      viewport.paneSize = Vector2d(vp.paneSizeW, vp.paneSizeH);
      viewport.devicePixelRatio = vp.devicePixelRatio;
      viewport.zoom = vp.zoom;
      viewport.panDocPoint = Vector2d(vp.panDocX, vp.panDocY);
      viewport.panScreenPoint = Vector2d(vp.panScreenX, vp.panScreenY);
      viewport.documentViewBox =
          Box2d::FromXYWH(vp.viewBoxX, vp.viewBoxY, vp.viewBoxW, vp.viewBoxH);
    }

    const bool hasDoc = frame.mouseDocX.has_value() && frame.mouseDocY.has_value();
    const Vector2d mouseDoc =
        hasDoc ? Vector2d(*frame.mouseDocX, *frame.mouseDocY)
               : viewport.screenToDocument(Vector2d(frame.mouseX, frame.mouseY));
    const bool nowHeld = (frame.mouseButtonMask & 1) != 0;

    for (const auto& ev : frame.events) {
      if (ev.kind == repro::ReproEvent::Kind::MouseDown && ev.mouseButton == 0) {
        if (!renderCoordinator.asyncRenderer().isBusy()) {
          selectTool.onMouseDown(app, mouseDoc, /*modifiers=*/{});
          results.mouseDownFrameIndices.push_back(frame.index);
        }
      } else if (ev.kind == repro::ReproEvent::Kind::MouseUp && ev.mouseButton == 0) {
        selectTool.onMouseUp(app, mouseDoc);
        results.mouseUpFrameIndices.push_back(frame.index);
        std::string tag;
        std::string id;
        std::optional<Box2d> worldBounds;
        if (app.selectedElement().has_value() &&
            app.selectedElement()->isa<svg::SVGGraphicsElement>()) {
          const svg::SVGElement sel = *app.selectedElement();
          tag = sel.tagName().name.str();
          id = std::string(sel.id());
          // Union renderable-geometry bounds while the registry is
          // live. Filter-group selections sit on a `<g filter=…>`
          // whose own bounds are empty; the subtree's leaves carry
          // the actual visible geometry.
          for (const auto& geometry : CollectRenderableGeometry(sel)) {
            const auto wb = geometry.worldBounds();
            if (!wb.has_value()) continue;
            if (worldBounds.has_value()) {
              worldBounds->addBox(*wb);
            } else {
              worldBounds = *wb;
            }
          }
        }
        results.selectionElementTags.push_back(std::move(tag));
        results.selectionElementIds.push_back(std::move(id));
        results.selectionWorldBoundsAtMouseUp.push_back(worldBounds);
      }
    }

    if (nowHeld && leftHeld) {
      selectTool.onMouseMove(app, mouseDoc, /*buttonHeld=*/true);
    }
    leftHeld = nowHeld;

    const auto flushStart = std::chrono::steady_clock::now();
    timing.wasBusy = renderCoordinator.asyncRenderer().isBusy();
    if (!timing.wasBusy) {
      (void)app.flushFrame();
    }
    timing.flushFrameMs = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - flushStart)
                             .count();

    // On-demand rendering — mirror the live editor's
    // `glfwWaitEvents`-driven loop. A frame is worth rendering
    // when something about the scene is actually changing:
    //   - first frame overall (cold render → baseline flat bitmap),
    //   - any discrete event (mouse down/up, resize, key) fired,
    //   - a drag is in flight (mouse held → DOM mutated this frame
    //     by `SelectTool::onMouseMove` → scene changed),
    //   - caller requested a checkpoint for this frame.
    //
    // Skipping idle-frame renders takes the replay from ~4 minutes
    // on a 900-frame recording (one full-document compose per
    // frame, ~400 ms on the splash) down to the ~5 seconds a real
    // editor session takes, with the same correctness: idle frames
    // produce no DOM mutations, so no render output would differ
    // from the prior frame anyway.
    const bool hasEvents = !frame.events.empty();
    const bool isCheckpoint = checkpointSet.contains(frame.index);
    const bool isFirstFrame = results.frames.empty();
    const bool shouldRender = isFirstFrame || hasEvents || nowHeld || isCheckpoint;

    std::optional<RenderResult> renderOpt;
    if (shouldRender) {
      renderOpt = DrainOneRenderFull(renderCoordinator.asyncRenderer(),
                                     renderCoordinator.renderer(), app.document().document(),
                                     app, selectTool, version);
      ++version;
      timing.workerMs = renderOpt.has_value() ? renderOpt->workerMs : -1.0;
    } else {
      timing.workerMs = -1.0;  // skipped
    }
    timing.totalFrameMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - frameStart)
                              .count();

    if (renderOpt.has_value()) {
      if (!results.preDragRender.has_value() && results.mouseDownFrameIndices.empty()) {
        results.preDragRender = *renderOpt;
      }
      results.finalRender = *renderOpt;
    }

    if (isCheckpoint && config.onCheckpoint) {
      const RenderResult* resultPtr = renderOpt.has_value() ? &*renderOpt : nullptr;
      config.onCheckpoint(nextCheckpointIdx++, resultPtr);
    }

    results.frames.push_back(timing);
  }

  results.canvasFromDocumentAtEnd = app.document().document().canvasFromDocumentTransform();

  // Post-replay settle render. See `ReplayConfig::appendSettleFrame`
  // docs for the rationale: the compositor keeps its split-layer
  // cache alive as long as the selection is held, which freezes the
  // flat bitmap at pre-first-drag state. Clearing the selection +
  // one more render demotes the entity, and the subsequent
  // `composeLayers` does a full flat re-rasterize of the post-drag
  // DOM state. That bitmap is what `settleRender` carries back.
  if (config.appendSettleFrame) {
    app.clearSelection();
    (void)app.flushFrame();
    auto settleOpt = DrainOneRenderFull(renderCoordinator.asyncRenderer(),
                                        renderCoordinator.renderer(), app.document().document(),
                                        app, selectTool, version);
    if (settleOpt.has_value()) {
      results.settleRender = std::move(*settleOpt);
    }
  }

  const auto counters = renderCoordinator.asyncRenderer().compositorFastPathCountersForTesting();
  results.fastPathFrames = counters.fastPathFrames;
  results.slowPathFramesWithDirty = counters.slowPathFramesWithDirty;
  results.noDirtyFrames = counters.noDirtyFrames;
  return results;
}

}  // namespace donner::editor
