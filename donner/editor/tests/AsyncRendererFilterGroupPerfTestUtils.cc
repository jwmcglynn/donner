#include "donner/editor/tests/AsyncRendererFilterGroupPerfTestUtils.h"

#include <chrono>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include "donner/base/Transform.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/AsyncSVGDocument.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/renderer/Renderer.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

std::optional<RenderResult> WaitForResult(AsyncRenderer* asyncRenderer) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    auto result = asyncRenderer->pollResult();
    if (result.has_value()) {
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  return std::nullopt;
}

}  // namespace

void RunFilterGroupSubtreeDragPerfScenario(FilterGroupSubtreeDragPerfResult* result) {
  ASSERT_NE(result, nullptr);

  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }

  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();
  ASSERT_FALSE(splashSource.empty());

  AsyncSVGDocument asyncDoc;
  ASSERT_TRUE(asyncDoc.loadFromString(splashSource));
  asyncDoc.document().setCanvasSize(892, 512);
  svg::SVGDocument& document = asyncDoc.document();

  auto targetElement = document.querySelector("#Big_lightning_glow");
  ASSERT_TRUE(targetElement.has_value());
  const Entity targetEntity = targetElement->entityHandle().entity();

  svg::Renderer renderer;
  AsyncRenderer asyncRenderer;

  const auto postRequest = [&](uint64_t version, bool activeDrag) {
    RenderRequest request;
    request.renderer = &renderer;
    request.document = &document;
    request.version = version;
    request.documentGeneration = 1;
    request.selectedEntity = targetEntity;
    request.dragPreview = RenderRequest::DragPreview{
        .entity = targetEntity,
        .interactionKind = activeDrag ? svg::compositor::InteractionHint::ActiveDrag
                                      : svg::compositor::InteractionHint::Selection,
    };
    asyncRenderer.requestRender(request);
  };

  postRequest(/*version=*/1, /*activeDrag=*/false);
  ASSERT_TRUE(WaitForResult(&asyncRenderer).has_value());

  constexpr int kDragFrames = 10;
  double maxDragFrameMs = 0.0;
  double sumDragFrameMs = 0.0;
  for (int i = 0; i < kDragFrames; ++i) {
    targetElement->cast<svg::SVGGraphicsElement>().setTransform(
        Transform2d::Translate(Vector2d(static_cast<double>(i + 1) * 3.0, 0.0)));
    const auto start = std::chrono::steady_clock::now();
    postRequest(/*version=*/static_cast<uint64_t>(i + 2), /*activeDrag=*/true);
    auto renderResult = WaitForResult(&asyncRenderer);
    const double frameMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    ASSERT_TRUE(renderResult.has_value()) << "filter-group drag frame " << i << " didn't land";
    ASSERT_TRUE(renderResult->compositedPreview.has_value())
        << "filter-group drag frame " << i
        << " produced no composited preview — compositor fell through to the "
           "non-composited path and will be slow for every subsequent frame";
    sumDragFrameMs += frameMs;
    maxDragFrameMs = std::max(maxDragFrameMs, frameMs);
  }

  const auto counters = asyncRenderer.compositorFastPathCountersForTesting();
  result->avgDragFrameMs = sumDragFrameMs / kDragFrames;
  result->maxDragFrameMs = maxDragFrameMs;
  result->dragFrames = kDragFrames;
  result->fastPathFrames = counters.fastPathFrames;
  result->slowPathFramesWithDirty = counters.slowPathFramesWithDirty;

  std::cerr << "[PERF] DraggingFilterGroupSubtree: avg=" << result->avgDragFrameMs
            << " ms, max=" << result->maxDragFrameMs << " ms over " << kDragFrames
            << " frames; fast=" << counters.fastPathFrames
            << " slowWithDirty=" << counters.slowPathFramesWithDirty
            << " noDirty=" << counters.noDirtyFrames << "\n";
}

}  // namespace donner::editor
