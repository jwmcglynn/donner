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
  EXPECT_TRUE(result->bitmap.empty());
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

}  // namespace
}  // namespace donner::editor
