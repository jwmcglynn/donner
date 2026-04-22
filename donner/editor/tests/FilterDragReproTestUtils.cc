/// @file
///
/// Shared replay harness implementation for the `FilterDragRepro_*`
/// tests. See `FilterDragReproTestUtils.h` for the contract.
///
/// Replay is high-fidelity at the input boundary: we read mouse-down,
/// mouse-move, and mouse-up events out of the `.donner-repro` file and
/// drive `SelectTool` + `EditorApp` with the document-space coordinates
/// derived from the recording's window geometry. Every repro frame
/// maps to one `runFrame()` tick that drains the command queue, fires
/// an async render request, and waits for the bitmap to land — exactly
/// the flow `main.cc` runs per-frame.
///
/// Design-doc context: this is the first vertical slice of Stage 2 of
/// `docs/design_docs/0029-ui_input_repro.md` (headless replay) —
/// scoped tightly to the repro we have in hand rather than a general
/// replay player. When Stage 2 lands in full this harness collapses
/// into `donner::editor::repro::ReplayPlayer`.

#include "donner/editor/tests/FilterDragReproTestUtils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "donner/base/EcsRegistry_fwd.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/RenderCoordinator.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/ViewportState.h"
#include "donner/editor/repro/ReproFile.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/compositor/ScopedCompositorHint.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "gtest/gtest.h"

namespace donner::editor::filter_drag_repro {
namespace {

using ::donner::Entity;
using ::donner::editor::AsyncRenderer;
using ::donner::editor::EditorApp;
using ::donner::editor::RenderCoordinator;
using ::donner::editor::RenderRequest;
using ::donner::editor::SelectTool;
using ::donner::editor::ViewportState;

// ---------------------------------------------------------------------------
// Editor pane layout constants — mirrors `EditorShell.cc`.
//
// The recording was captured with these exact offsets, so any drift here
// would dislocate the replayed clicks from the elements the user hit in
// the live editor. Keep in sync with `EditorShell.cc` — if the shell
// layout changes, replay-repro tests need to be re-recorded.
// ---------------------------------------------------------------------------

constexpr double kSourcePaneWidth = 560.0;
constexpr double kInspectorPaneWidth = 320.0;
// Typical ImGui menu-bar height under the project's style; matches the
// live editor's top-of-pane offset within ~1px across platforms.
constexpr double kMenuBarHeight = 20.0;

struct FrameTiming {
  uint64_t reproFrameIndex = 0;
  double flushFrameMs = 0.0;
  double workerMs = 0.0;
  double totalFrameMs = 0.0;
  bool wasBusy = false;
};

// Mirrors `EditorShell::runFrame`'s render-pane layout derivation (see
// `EditorShell.cc`'s pane-origin math just before
// `renderRenderPane()` — same subtraction with the same constants).
Vector2d RenderPaneOriginForWindow() {
  return Vector2d(kSourcePaneWidth, kMenuBarHeight);
}
Vector2d RenderPaneSizeForWindow(double windowW, double windowH) {
  return Vector2d(std::max(0.0, windowW - kSourcePaneWidth - kInspectorPaneWidth),
                  std::max(0.0, windowH - kMenuBarHeight));
}

std::string LoadFileOrSkip(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return {};
  std::ostringstream buf;
  buf << file.rdbuf();
  return buf.str();
}

// Drain one async render: request, poll until result lands, return the
// worker-reported render time. Times out after a generous deadline so a
// truly-stuck worker is caught with a clear failure instead of the whole
// test run hanging.
std::optional<double> DrainOneRender(AsyncRenderer& asyncRenderer, svg::Renderer& renderer,
                                     svg::SVGDocument& document, EditorApp& editorApp,
                                     SelectTool& selectTool, uint64_t version) {
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
      return result->workerMs;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return std::nullopt;
}

// Track the held-button state across frames so we can synthesize
// `onMouseMove` events between the recorded down/up frames — the repro
// captures mouse POSITION every frame but only emits discrete events at
// button edges. The live editor fires `onMouseMove` every frame the
// button is held at whatever position ImGui reports, so faithful replay
// needs the same per-frame pump.
struct ReplayState {
  bool leftButtonHeld = false;
};

struct ReplayResults {
  std::vector<FrameTiming> frames;
  std::vector<uint64_t> mouseDownFrameIndices;
  std::vector<uint64_t> mouseUpFrameIndices;
  std::vector<Entity> selectedEntityAtFrame;
  std::vector<Entity> selectionAfterMouseUp;
  std::vector<std::string> selectionElementIds;
  std::vector<std::string> selectionFilterAncestorIds;
  uint64_t fastPathFrames = 0;
  uint64_t slowPathFramesWithDirty = 0;
  uint64_t noDirtyFrames = 0;
};

ReplayResults ReplayRepro(const std::filesystem::path& reproPath,
                          const std::filesystem::path& svgPath) {
  ReplayResults results;

  const std::string svgSource = LoadFileOrSkip(svgPath);
  [&]() { ASSERT_FALSE(svgSource.empty()) << "svg source missing: " << svgPath; }();
  auto reproOpt = repro::ReadReproFile(reproPath);
  [&]() { ASSERT_TRUE(reproOpt.has_value()) << "repro file parse failed: " << reproPath; }();
  const repro::ReproFile& repro = *reproOpt;

  EditorApp app;
  [&]() { ASSERT_TRUE(app.loadFromString(svgSource)) << "loadFromString on splash failed"; }();

  SelectTool selectTool;
  // The recording was captured with `--experimental`, which in the live
  // shell flips `SelectTool::setCompositedDragPreviewEnabled(true)` so
  // `activeDragPreview()` returns a valid payload and the drag path
  // flows through the compositor. Replay has to mirror that or the
  // drag requests arrive with only a Selection-kind hint (not
  // ActiveDrag), which disables the `skipMainCompose` fast path in
  // `composeLayers` and forces a full canvas compose every frame.
  selectTool.setCompositedDragPreviewEnabled(repro.metadata.experimentalMode);

  RenderCoordinator renderCoordinator;

  const double windowW = static_cast<double>(repro.metadata.windowWidth);
  const double windowH = static_cast<double>(repro.metadata.windowHeight);
  ViewportState viewport;
  viewport.paneOrigin = RenderPaneOriginForWindow();
  viewport.paneSize = RenderPaneSizeForWindow(windowW, windowH);
  viewport.devicePixelRatio = repro.metadata.displayScale;
  viewport.documentViewBox = *app.document().document().svgElement().viewBox();
  viewport.resetTo100Percent();

  app.document().document().setCanvasSize(viewport.desiredCanvasSize().x,
                                          viewport.desiredCanvasSize().y);

  ReplayState state;
  uint64_t version = 1;

  for (const auto& frame : repro.frames) {
    FrameTiming timing;
    timing.reproFrameIndex = frame.index;

    const auto frameStart = std::chrono::steady_clock::now();

    const Vector2d mouseScreen(frame.mouseX, frame.mouseY);
    const Vector2d mouseDoc = viewport.screenToDocument(mouseScreen);
    const bool wasHeld = state.leftButtonHeld;
    const bool nowHeld = (frame.mouseButtonMask & 1) != 0;

    for (const auto& e : frame.events) {
      if (e.kind == repro::ReproEvent::Kind::MouseDown && e.mouseButton == 0) {
        if (!renderCoordinator.asyncRenderer().isBusy()) {
          selectTool.onMouseDown(app, mouseDoc, /*modifiers=*/{});
          results.mouseDownFrameIndices.push_back(frame.index);
        }
      } else if (e.kind == repro::ReproEvent::Kind::MouseUp && e.mouseButton == 0) {
        selectTool.onMouseUp(app, mouseDoc);
        results.mouseUpFrameIndices.push_back(frame.index);
        std::string selId = "<none>";
        std::string filterAncestorId = "<none>";
        Entity postSelect = entt::null;
        if (app.selectedElement().has_value() &&
            app.selectedElement()->isa<svg::SVGGraphicsElement>()) {
          const svg::SVGElement sel = *app.selectedElement();
          postSelect = sel.entityHandle().entity();
          selId = std::string(sel.id());
          svg::SVGElement cursor = sel;
          while (auto parent = cursor.parentElement()) {
            if (parent->hasAttribute(xml::XMLQualifiedNameRef("filter"))) {
              filterAncestorId = std::string(parent->id());
              if (filterAncestorId.empty()) filterAncestorId = "<filter-ancestor-no-id>";
              break;
            }
            cursor = *parent;
          }
        }
        results.selectionAfterMouseUp.push_back(postSelect);
        results.selectionElementIds.push_back(std::move(selId));
        results.selectionFilterAncestorIds.push_back(std::move(filterAncestorId));
      }
    }

    if (nowHeld && wasHeld) {
      selectTool.onMouseMove(app, mouseDoc, /*buttonHeld=*/true);
    } else if (!nowHeld && wasHeld) {
      // Button just released — already dispatched above via the
      // discrete MouseUp event.
    }
    state.leftButtonHeld = nowHeld;

    const auto flushStart = std::chrono::steady_clock::now();
    timing.wasBusy = renderCoordinator.asyncRenderer().isBusy();
    if (!timing.wasBusy) {
      (void)app.flushFrame();
    }
    timing.flushFrameMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - flushStart)
            .count();

    auto workerMs = DrainOneRender(renderCoordinator.asyncRenderer(), renderCoordinator.renderer(),
                                   app.document().document(), app, selectTool, version);
    ++version;
    timing.workerMs = workerMs.value_or(-1.0);
    timing.totalFrameMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart)
            .count();

    const Entity sel =
        app.selectedElement().has_value() && app.selectedElement()->isa<svg::SVGGraphicsElement>()
            ? app.selectedElement()->entityHandle().entity()
            : entt::null;
    results.selectedEntityAtFrame.push_back(sel);

    results.frames.push_back(timing);
  }

  const auto counters = renderCoordinator.asyncRenderer().compositorFastPathCountersForTesting();
  results.fastPathFrames = counters.fastPathFrames;
  results.slowPathFramesWithDirty = counters.slowPathFramesWithDirty;
  results.noDirtyFrames = counters.noDirtyFrames;
  return results;
}

DragStats ComputeDragStats(const ReplayResults& r, uint64_t fromFrame, uint64_t toFrame) {
  DragStats stats;
  double sum = 0.0;
  for (const auto& f : r.frames) {
    if (f.reproFrameIndex > fromFrame && f.reproFrameIndex < toFrame && f.workerMs >= 0.0) {
      sum += f.workerMs;
      stats.maxWorkerMs = std::max(stats.maxWorkerMs, f.workerMs);
      ++stats.frameCount;
    }
  }
  if (stats.frameCount > 0) {
    stats.avgWorkerMs = sum / stats.frameCount;
  }
  return stats;
}

void DumpHistogram(const ReplayResults& r, uint64_t fromFrame, uint64_t toFrame,
                   const char* label) {
  int buckets[6] = {0};  // <5, 5-15, 15-30, 30-60, 60-120, >=120
  for (const auto& f : r.frames) {
    if (f.reproFrameIndex <= fromFrame || f.reproFrameIndex >= toFrame || f.workerMs < 0.0) {
      continue;
    }
    if (f.workerMs < 5)
      buckets[0]++;
    else if (f.workerMs < 15)
      buckets[1]++;
    else if (f.workerMs < 30)
      buckets[2]++;
    else if (f.workerMs < 60)
      buckets[3]++;
    else if (f.workerMs < 120)
      buckets[4]++;
    else
      buckets[5]++;
  }
  std::cerr << "[FilterDragRepro] " << label << " worker-ms histogram:";
  std::cerr << " <5=" << buckets[0];
  std::cerr << " 5-15=" << buckets[1];
  std::cerr << " 15-30=" << buckets[2];
  std::cerr << " 30-60=" << buckets[3];
  std::cerr << " 60-120=" << buckets[4];
  std::cerr << " >=120=" << buckets[5] << "\n";
}

void DumpFirstFiveFrames(const ReplayResults& r, uint64_t fromFrame, uint64_t toFrame,
                         const char* label) {
  std::cerr << "[FilterDragRepro] " << label << " first 5 frames: ";
  int printed = 0;
  for (const auto& f : r.frames) {
    if (f.reproFrameIndex <= fromFrame || f.reproFrameIndex >= toFrame || f.workerMs < 0.0) {
      continue;
    }
    if (printed++ >= 5) break;
    std::cerr << f.workerMs << "ms ";
  }
  std::cerr << "\n";
}

}  // namespace

void RunFilterDragReproScenario(FilterDragReproResult* out) {
  const std::filesystem::path reproPath = "donner/editor/tests/filter_drag_repro.rnr";
  const std::filesystem::path svgPath = "donner_splash.svg";

  if (!std::filesystem::exists(reproPath) || !std::filesystem::exists(svgPath)) {
    out->skipped = true;
    return;
  }

  ReplayResults r = ReplayRepro(reproPath, svgPath);
  ASSERT_FALSE(r.frames.empty()) << "repro produced zero frames";
  ASSERT_EQ(r.mouseDownFrameIndices.size(), 2u)
      << "expected exactly two mouse-down events in the repro (first drag, second drag)";
  ASSERT_EQ(r.mouseUpFrameIndices.size(), 2u) << "expected two mouse-up events in the repro";

  const uint64_t firstDown = r.mouseDownFrameIndices[0];
  const uint64_t firstUp = r.mouseUpFrameIndices[0];
  const uint64_t secondDown = r.mouseDownFrameIndices[1];
  const uint64_t secondUp = r.mouseUpFrameIndices[1];

  out->firstDrag = ComputeDragStats(r, firstDown, firstUp);
  out->secondDrag = ComputeDragStats(r, secondDown, secondUp);
  ASSERT_GT(out->firstDrag.frameCount, 0);
  ASSERT_GT(out->secondDrag.frameCount, 0);

  std::cerr << "[FilterDragRepro] first drag: frames=" << out->firstDrag.frameCount
            << ", avg=" << out->firstDrag.avgWorkerMs << " ms, max=" << out->firstDrag.maxWorkerMs
            << " ms\n";
  std::cerr << "[FilterDragRepro] second drag: frames=" << out->secondDrag.frameCount
            << ", avg=" << out->secondDrag.avgWorkerMs << " ms, max=" << out->secondDrag.maxWorkerMs
            << " ms\n";

  DumpHistogram(r, firstDown, firstUp, "first-drag");
  DumpHistogram(r, secondDown, secondUp, "second-drag");
  DumpFirstFiveFrames(r, firstDown, firstUp, "first-drag");
  DumpFirstFiveFrames(r, secondDown, secondUp, "second-drag");
  std::cerr << "[FilterDragRepro] fast-path counters at end: fast=" << r.fastPathFrames
            << " slowWithDirty=" << r.slowPathFramesWithDirty << " noDirty=" << r.noDirtyFrames
            << "\n";
  ASSERT_EQ(r.selectionElementIds.size(), 2u);
  std::cerr << "[FilterDragRepro] firstSel id=" << r.selectionElementIds[0]
            << " filterAncestor=" << r.selectionFilterAncestorIds[0] << "\n";
  std::cerr << "[FilterDragRepro] secondSel id=" << r.selectionElementIds[1]
            << " filterAncestor=" << r.selectionFilterAncestorIds[1] << "\n";

  ASSERT_EQ(r.selectionAfterMouseUp.size(), 2u);
  const Entity firstSel = r.selectionAfterMouseUp[0];
  const Entity secondSel = r.selectionAfterMouseUp[1];
  out->firstSelectionExists = (firstSel != entt::null);
  out->secondSelectionExists = (secondSel != entt::null);
  out->selectionChangedAcrossDrags = (firstSel != secondSel);
  out->firstSelectionId = r.selectionElementIds[0];
  out->firstSelectionFilterAncestorId = r.selectionFilterAncestorIds[0];
  out->secondSelectionId = r.selectionElementIds[1];
  out->secondSelectionFilterAncestorId = r.selectionFilterAncestorIds[1];

  out->fastPathFrames = r.fastPathFrames;
  out->slowPathFramesWithDirty = r.slowPathFramesWithDirty;
  out->noDirtyFrames = r.noDirtyFrames;
}

}  // namespace donner::editor::filter_drag_repro
