/// @file
/// GeodeSurfaceRenderer over a real Core Animation layer: creation, presentation, resize and
/// argument validation of the public application-embedding entry point.

#import <QuartzCore/CAMetalLayer.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/GeodeSurfaceRenderer.h"

namespace donner::svg {
namespace {

using testing::Eq;
using testing::HasSubstr;
using testing::IsNull;
using testing::NotNull;

/// A surface frame can be briefly unavailable; this bounds the retries before a frame must present.
constexpr int kMaxPresentAttempts = 32;

SVGDocument ParseDocument() {
  ParseWarningSink warnings;
  ParseResult<SVGDocument> parsed = parser::SVGParser::ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
           <rect width="64" height="64" fill="#d33"/>
           <circle cx="32" cy="32" r="16" fill="#3d3"/>
         </svg>)",
      warnings);
  EXPECT_FALSE(parsed.hasError()) << parsed.error();
  return std::move(parsed.result());
}

/// Draws until a frame presents or the attempt budget runs out, returning the last status.
GeodeSurfaceRenderer::FrameStatus DrawUntilPresented(GeodeSurfaceRenderer& renderer,
                                                     SVGDocument& document) {
  GeodeSurfaceRenderer::FrameStatus status = GeodeSurfaceRenderer::FrameStatus::Skipped;
  for (int attempt = 0; attempt < kMaxPresentAttempts; ++attempt) {
    status = renderer.draw(document);
    if (status != GeodeSurfaceRenderer::FrameStatus::Skipped) {
      break;
    }
  }
  return status;
}

class GeodeSurfaceRendererTest : public testing::Test {
protected:
  void SetUp() override {
    layer_ = [CAMetalLayer layer];
    layer_.contentsScale = 1.0;
  }

  void* layer() { return (__bridge void*)layer_; }

  CAMetalLayer* layer_ = nil;
};

TEST_F(GeodeSurfaceRendererTest, PresentsADocumentToAMetalLayer) {
  std::string error;
  std::unique_ptr<GeodeSurfaceRenderer> renderer =
      GeodeSurfaceRenderer::CreateForMetalLayer(layer(), Vector2i(64, 64), &error);
  ASSERT_THAT(renderer, NotNull()) << error;
  EXPECT_THAT(renderer->size(), Eq(Vector2i(64, 64)));

  SVGDocument document = ParseDocument();
  document.setCanvasSize(64, 64);
  EXPECT_THAT(DrawUntilPresented(*renderer, document),
              Eq(GeodeSurfaceRenderer::FrameStatus::Presented));
  // Presenting again reuses the surface and device.
  EXPECT_THAT(DrawUntilPresented(*renderer, document),
              Eq(GeodeSurfaceRenderer::FrameStatus::Presented));
  EXPECT_FALSE(renderer->isDeviceLost());
}

TEST_F(GeodeSurfaceRendererTest, ResizeReconfiguresTheSurface) {
  std::string error;
  std::unique_ptr<GeodeSurfaceRenderer> renderer =
      GeodeSurfaceRenderer::CreateForMetalLayer(layer(), Vector2i(64, 64), &error);
  ASSERT_THAT(renderer, NotNull()) << error;

  ASSERT_TRUE(renderer->resize(Vector2i(96, 48)));
  EXPECT_THAT(renderer->size(), Eq(Vector2i(96, 48)));

  SVGDocument document = ParseDocument();
  document.setCanvasSize(96, 48);
  EXPECT_THAT(DrawUntilPresented(*renderer, document),
              Eq(GeodeSurfaceRenderer::FrameStatus::Presented));

  EXPECT_FALSE(renderer->resize(Vector2i(0, 48))) << "an empty size is rejected";
  EXPECT_THAT(renderer->size(), Eq(Vector2i(96, 48))) << "the previous size stays in effect";
}

TEST_F(GeodeSurfaceRendererTest, RejectsANullLayer) {
  std::string error;
  EXPECT_THAT(GeodeSurfaceRenderer::CreateForMetalLayer(nullptr, Vector2i(64, 64), &error),
              IsNull());
  EXPECT_THAT(error, HasSubstr("null"));
}

TEST_F(GeodeSurfaceRendererTest, RejectsAnEmptySize) {
  std::string error;
  EXPECT_THAT(GeodeSurfaceRenderer::CreateForMetalLayer(layer(), Vector2i(64, 0), &error),
              IsNull());
  EXPECT_THAT(error, HasSubstr("positive"));
  // The error out-parameter is optional.
  EXPECT_THAT(GeodeSurfaceRenderer::CreateForMetalLayer(layer(), Vector2i(-1, 64)), IsNull());
}

}  // namespace
}  // namespace donner::svg
