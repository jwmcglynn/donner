#include "donner/editor/AsyncRenderer.h"

#include <chrono>
#include <thread>

#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/tests/ParserTestUtils.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

TEST(AsyncRendererTest, DragPreviewRequestReturnsCompositedPreviewLayers) {
  svg::SVGDocument document = svg::instantiateSubtree(R"svg(
    <rect id="under" x="0" y="0" width="16" height="16" fill="blue" />
    <rect id="target" x="20" y="0" width="16" height="16" fill="red" />
    <rect id="over" x="40" y="0" width="16" height="16" fill="green" />
  )svg");
  document.setCanvasSize(64, 64);

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  svg::Renderer renderer;
  AsyncRenderer asyncRenderer;

  RenderRequest request;
  request.renderer = &renderer;
  request.document = &document;
  request.version = 1;
  request.dragPreview = RenderRequest::DragPreview{
      .entity = target->entityHandle().entity(),
      .translation = Vector2d(8.0, 4.0),
  };

  asyncRenderer.requestRender(request);

  std::optional<RenderResult> result;
  for (int i = 0; i < 200 && !result.has_value(); ++i) {
    result = asyncRenderer.pollResult();
    if (!result.has_value()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->compositedPreview.has_value());
  EXPECT_TRUE(result->compositedPreview->valid());
  // The flat fallback bitmap is always produced (even for composited renders) so the main loop can
  // keep its flat texture current.
  EXPECT_FALSE(result->bitmap.empty());
  EXPECT_EQ(result->compositedPreview->entity, target->entityHandle().entity());
  EXPECT_FALSE(result->compositedPreview->promotedBitmap.empty());
}

TEST(AsyncRendererTest, ZeroTranslationPreviewRequestReturnsCompositedPreviewLayers) {
  svg::SVGDocument document = svg::instantiateSubtree(R"svg(
    <rect id="under" x="0" y="0" width="16" height="16" fill="blue" />
    <rect id="target" x="20" y="0" width="16" height="16" fill="red" />
    <rect id="over" x="40" y="0" width="16" height="16" fill="green" />
  )svg");
  document.setCanvasSize(64, 64);

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  svg::Renderer renderer;
  AsyncRenderer asyncRenderer;

  RenderRequest request;
  request.renderer = &renderer;
  request.document = &document;
  request.version = 1;
  request.dragPreview = RenderRequest::DragPreview{
      .entity = target->entityHandle().entity(),
      .translation = Vector2d::Zero(),
  };

  asyncRenderer.requestRender(request);

  std::optional<RenderResult> result;
  for (int i = 0; i < 200 && !result.has_value(); ++i) {
    result = asyncRenderer.pollResult();
    if (!result.has_value()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->compositedPreview.has_value());
  EXPECT_TRUE(result->compositedPreview->valid());
}

TEST(AsyncRendererTest, CompositorResetOnDocumentVersionChange) {
  svg::SVGDocument document = svg::instantiateSubtree(R"svg(
    <rect id="under" x="0" y="0" width="16" height="16" fill="blue" />
    <rect id="target" x="20" y="0" width="16" height="16" fill="red" />
    <rect id="over" x="40" y="0" width="16" height="16" fill="green" />
  )svg");
  document.setCanvasSize(64, 64);

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  svg::Renderer renderer;
  AsyncRenderer asyncRenderer;

  // First render at version 1.
  {
    RenderRequest request;
    request.renderer = &renderer;
    request.document = &document;
    request.version = 1;
    request.dragPreview = RenderRequest::DragPreview{
        .entity = target->entityHandle().entity(),
        .translation = Vector2d(1.0, 0.0),
    };

    asyncRenderer.requestRender(request);

    std::optional<RenderResult> result;
    for (int i = 0; i < 200 && !result.has_value(); ++i) {
      result = asyncRenderer.pollResult();
      if (!result.has_value()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->compositedPreview.has_value());
  }

  // Second render at version 2 — compositor should reset rather than using stale layers.
  {
    RenderRequest request;
    request.renderer = &renderer;
    request.document = &document;
    request.version = 2;
    request.dragPreview = RenderRequest::DragPreview{
        .entity = target->entityHandle().entity(),
        .translation = Vector2d(2.0, 0.0),
    };

    asyncRenderer.requestRender(request);

    std::optional<RenderResult> result;
    for (int i = 0; i < 200 && !result.has_value(); ++i) {
      result = asyncRenderer.pollResult();
      if (!result.has_value()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    ASSERT_TRUE(result.has_value());
    // After a version change, the compositor should still produce valid composited output.
    ASSERT_TRUE(result->compositedPreview.has_value());
    EXPECT_TRUE(result->compositedPreview->valid());
    // The flat bitmap is also always produced.
    EXPECT_FALSE(result->bitmap.empty());
  }
}

// A request with `selectedEntity` set and no `dragPreview` must still
// produce a valid flat bitmap. This is the "compositor stays warm against
// the selection while no drag is happening" path — exercised when the user
// has just selected an element, or when a drag has released and the
// selection is being held ready for the next drag.
TEST(AsyncRendererTest, SelectedEntityWithoutDragPreviewProducesFlatBitmap) {
  svg::SVGDocument document = svg::instantiateSubtree(R"svg(
    <rect id="target" x="0" y="0" width="16" height="16" fill="red" />
  )svg");
  document.setCanvasSize(64, 64);

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  svg::Renderer renderer;
  AsyncRenderer asyncRenderer;

  RenderRequest request;
  request.renderer = &renderer;
  request.document = &document;
  request.version = 1;
  request.selectedEntity = target->entityHandle().entity();
  // No dragPreview — editor is holding a selection pre-warmed but not
  // dragging yet.

  asyncRenderer.requestRender(request);

  std::optional<RenderResult> result;
  for (int i = 0; i < 200 && !result.has_value(); ++i) {
    result = asyncRenderer.pollResult();
    if (!result.has_value()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->bitmap.empty());
  // compositedPreview is only exposed when the editor is actively dragging
  // (it's what drives the GPU-textures overlay). Pre-warm without drag just
  // produces the flat bitmap.
  EXPECT_FALSE(result->compositedPreview.has_value());
}

// After a drag → release → drag-again sequence on the same entity, the
// second drag's promoted bitmap must be bit-exactly identical to the one
// produced while the first drag was in flight (same entity, same document
// version, same canvas size). That proves the compositor was not torn down
// between drags — a teardown would force re-rasterization from scratch.
TEST(AsyncRendererTest, CompositorStaysAliveAcrossDragRelease) {
  svg::SVGDocument document = svg::instantiateSubtree(R"svg(
    <rect id="target" x="0" y="0" width="16" height="16" fill="red" />
  )svg");
  document.setCanvasSize(64, 64);

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  svg::Renderer renderer;
  AsyncRenderer asyncRenderer;

  const auto waitForResult = [&]() {
    std::optional<RenderResult> result;
    for (int i = 0; i < 200 && !result.has_value(); ++i) {
      result = asyncRenderer.pollResult();
      if (!result.has_value()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    return result;
  };

  // Drag frame 1: translation (4, 0).
  {
    RenderRequest request;
    request.renderer = &renderer;
    request.document = &document;
    request.version = 1;
    request.selectedEntity = target->entityHandle().entity();
    request.dragPreview = RenderRequest::DragPreview{
        .entity = target->entityHandle().entity(),
        .translation = Vector2d(4.0, 0.0),
    };
    asyncRenderer.requestRender(request);
  }
  auto drag1 = waitForResult();
  ASSERT_TRUE(drag1.has_value());
  ASSERT_TRUE(drag1->compositedPreview.has_value());
  const std::vector<uint8_t> promotedPixelsDrag1 =
      drag1->compositedPreview->promotedBitmap.pixels;
  ASSERT_FALSE(promotedPixelsDrag1.empty());

  // Release: selection held but no drag.
  {
    RenderRequest request;
    request.renderer = &renderer;
    request.document = &document;
    request.version = 1;
    request.selectedEntity = target->entityHandle().entity();
    asyncRenderer.requestRender(request);
  }
  auto held = waitForResult();
  ASSERT_TRUE(held.has_value());
  EXPECT_FALSE(held->compositedPreview.has_value());

  // Drag frame 2: same entity, translation (4, 0) again. If the compositor
  // were torn down between the release and this drag, it would re-rasterize
  // — but the output would still be visually correct, so check the cheaper
  // proxy: the promoted-entity bitmap must be bit-identical.
  {
    RenderRequest request;
    request.renderer = &renderer;
    request.document = &document;
    request.version = 1;
    request.selectedEntity = target->entityHandle().entity();
    request.dragPreview = RenderRequest::DragPreview{
        .entity = target->entityHandle().entity(),
        .translation = Vector2d(4.0, 0.0),
    };
    asyncRenderer.requestRender(request);
  }
  auto drag2 = waitForResult();
  ASSERT_TRUE(drag2.has_value());
  ASSERT_TRUE(drag2->compositedPreview.has_value());
  EXPECT_EQ(drag2->compositedPreview->promotedBitmap.pixels, promotedPixelsDrag1)
      << "promoted bitmap should be identical after release → drag-again";
}

}  // namespace
}  // namespace donner::editor
