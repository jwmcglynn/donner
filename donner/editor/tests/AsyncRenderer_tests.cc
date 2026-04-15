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

}  // namespace
}  // namespace donner::editor
