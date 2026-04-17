#include "donner/svg/compositor/CompositorController.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <sstream>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/compositor/ComplexityBucketer.h"
#include "donner/svg/compositor/DragSession.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

namespace donner::svg::compositor {

namespace {

class MockRendererInterface : public RendererInterface {
public:
  MOCK_METHOD(void, draw, (SVGDocument & document), (override));
  MOCK_METHOD(int, width, (), (const, override));
  MOCK_METHOD(int, height, (), (const, override));
  MOCK_METHOD(void, beginFrame, (const RenderViewport& viewport), (override));
  MOCK_METHOD(void, endFrame, (), (override));
  MOCK_METHOD(void, setTransform, (const Transform2d& transform), (override));
  MOCK_METHOD(void, pushTransform, (const Transform2d& transform), (override));
  MOCK_METHOD(void, popTransform, (), (override));
  MOCK_METHOD(void, pushClip, (const ResolvedClip& clip), (override));
  MOCK_METHOD(void, popClip, (), (override));
  MOCK_METHOD(void, pushIsolatedLayer, (double opacity, MixBlendMode blendMode), (override));
  MOCK_METHOD(void, popIsolatedLayer, (), (override));
  MOCK_METHOD(void, pushFilterLayer,
              (const components::FilterGraph& filterGraph,
               const std::optional<Box2d>& filterRegion),
              (override));
  MOCK_METHOD(void, popFilterLayer, (), (override));
  MOCK_METHOD(void, pushMask, (const std::optional<Box2d>& maskBounds), (override));
  MOCK_METHOD(void, transitionMaskToContent, (), (override));
  MOCK_METHOD(void, popMask, (), (override));
  MOCK_METHOD(void, beginPatternTile, (const Box2d& tileRect, const Transform2d& targetFromPattern),
              (override));
  MOCK_METHOD(void, endPatternTile, (bool forStroke), (override));
  MOCK_METHOD(void, setPaint, (const PaintParams& paint), (override));
  MOCK_METHOD(void, drawPath, (const PathShape& path, const StrokeParams& stroke), (override));
  MOCK_METHOD(void, drawRect, (const Box2d& rect, const StrokeParams& stroke), (override));
  MOCK_METHOD(void, drawEllipse, (const Box2d& bounds, const StrokeParams& stroke), (override));
  MOCK_METHOD(void, drawImage, (const ImageResource& image, const ImageParams& params), (override));
  MOCK_METHOD(void, drawText,
              (Registry & registry, const components::ComputedTextComponent& text,
               const TextParams& params),
              (override));
  MOCK_METHOD(RendererBitmap, takeSnapshot, (), (const, override));
  MOCK_METHOD(std::unique_ptr<RendererInterface>, createOffscreenInstance, (), (const, override));

  /// Create a tiny non-empty bitmap for caching tests.
  static RendererBitmap makeDummyBitmap() {
    RendererBitmap bmp;
    bmp.dimensions = Vector2i(1, 1);
    bmp.rowBytes = 4;
    bmp.pixels = {0, 0, 0, 255};
    return bmp;
  }
};

/// Generate an SVG document with a grid of \p count rects.
std::string generateGridSvg(int count, int cols = 100) {
  std::ostringstream svgBuilder;
  svgBuilder << R"(<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="1000">)";

  const double cellW = 1000.0 / cols;
  const int rows = (count + cols - 1) / cols;
  const double cellH = 1000.0 / std::max(rows, 1);

  for (int i = 0; i < count; ++i) {
    const int col = i % cols;
    const int row = i / cols;
    const double x = col * cellW;
    const double y = row * cellH;

    // Give the first element an id for selection.
    if (i == 0) {
      svgBuilder << R"(<rect id="target" x=")" << x << R"(" y=")" << y << R"(" width=")"
                 << cellW * 0.9 << R"(" height=")" << cellH * 0.9
                 << R"(" fill="hsl()" << (i * 37) % 360 << R"(, 70%, 50%)" << R"(" />)";
    } else {
      svgBuilder << R"(<rect x=")" << x << R"(" y=")" << y << R"(" width=")" << cellW * 0.9
                 << R"(" height=")" << cellH * 0.9 << R"(" fill="hsl()" << (i * 37) % 360
                 << R"(, 70%, 50%)" << R"(" />)";
    }
  }

  svgBuilder << "</svg>";
  return svgBuilder.str();
}

using Clock = std::chrono::high_resolution_clock;

}  // namespace

class CompositorPerfTest : public ::testing::Test {
protected:
  SVGDocument makeDocument(std::string_view svg, Vector2i size = Vector2i(1000, 1000)) {
    return instantiateSubtree(svg, parser::SVGParser::Options(), size);
  }

  /// Configure the mock renderer to support offscreen instances and non-empty snapshots,
  /// enabling the compositor's caching logic.
  void configureMockForCaching() {
    ON_CALL(renderer_, takeSnapshot()).WillByDefault([]() {
      return MockRendererInterface::makeDummyBitmap();
    });
    ON_CALL(renderer_, createOffscreenInstance()).WillByDefault([this]() {
      auto offscreen = std::make_unique<NiceMock<MockRendererInterface>>();
      ON_CALL(*offscreen, takeSnapshot()).WillByDefault([]() {
        return MockRendererInterface::makeDummyBitmap();
      });
      ON_CALL(*offscreen, createOffscreenInstance()).WillByDefault([]() { return nullptr; });
      return offscreen;
    });
  }

  NiceMock<MockRendererInterface> renderer_;
};

// Measure compositor overhead per frame when dragging one shape in a large scene.
// This test measures the compositing infrastructure overhead (promote, set transform, compose),
// NOT the actual rasterization time (which uses mock renderer). The mock renderer returns
// immediately, isolating the compositor's own overhead.
TEST_F(CompositorPerfTest, DragFrameOverhead_1kNodes) {
  const std::string svg = generateGridSvg(1000, 32);
  SVGDocument document = makeDocument(svg);

  configureMockForCaching();
  CompositorController compositor(document, renderer_);

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  auto drag = DragSession::begin(compositor, entity);
  ASSERT_TRUE(drag.has_value());

  RenderViewport viewport;
  viewport.size = Vector2d(1000, 1000);
  viewport.devicePixelRatio = 1.0;

  // Warm up.
  for (int i = 0; i < 5; ++i) {
    drag->updateTranslation(Vector2d(i * 1.0, i * 0.5));
    compositor.renderFrame(viewport);
  }

  // Measure.
  constexpr int kIterations = 100;
  auto start = Clock::now();
  for (int i = 0; i < kIterations; ++i) {
    drag->updateTranslation(Vector2d(i * 2.0, i * 1.0));
    compositor.renderFrame(viewport);
  }
  auto end = Clock::now();

  const auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  const double avgMs = static_cast<double>(totalUs) / kIterations / 1000.0;

  // Report.
  std::cerr << "[PERF] DragFrameOverhead_1kNodes: " << avgMs << " ms/frame (avg over "
            << kIterations << " iterations, mock renderer)\n";

  // Compositor infrastructure overhead should be well under 1ms per frame with mock renderer.
  EXPECT_LT(avgMs, 1.0) << "Compositor overhead per frame exceeds 1ms (mock renderer)";
}

TEST_F(CompositorPerfTest, PromoteDemoteCycle_1kNodes) {
  const std::string svg = generateGridSvg(1000, 32);
  SVGDocument document = makeDocument(svg);

  CompositorController compositor(document, renderer_);

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  constexpr int kCycles = 1000;
  auto start = Clock::now();
  for (int i = 0; i < kCycles; ++i) {
    compositor.promoteEntity(entity);
    compositor.demoteEntity(entity);
  }
  auto end = Clock::now();

  const auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  const double avgUs = static_cast<double>(totalUs) / kCycles;

  std::cerr << "[PERF] PromoteDemoteCycle_1kNodes: " << avgUs << " us/cycle (avg over " << kCycles
            << " cycles)\n";

  // Promote/demote should be sub-millisecond.
  EXPECT_LT(avgUs, 1000.0) << "Promote/demote cycle exceeds 1ms";
}

TEST_F(CompositorPerfTest, DragFrameOverhead_10kNodes) {
  const std::string svg = generateGridSvg(10000, 100);
  SVGDocument document = makeDocument(svg);

  configureMockForCaching();
  CompositorController compositor(document, renderer_);

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  auto drag = DragSession::begin(compositor, entity);
  ASSERT_TRUE(drag.has_value());

  RenderViewport viewport;
  viewport.size = Vector2d(1000, 1000);
  viewport.devicePixelRatio = 1.0;

  // Warm up.
  for (int i = 0; i < 3; ++i) {
    drag->updateTranslation(Vector2d(i * 1.0, i * 0.5));
    compositor.renderFrame(viewport);
  }

  // Measure.
  constexpr int kIterations = 50;
  auto start = Clock::now();
  for (int i = 0; i < kIterations; ++i) {
    drag->updateTranslation(Vector2d(i * 2.0, i * 1.0));
    compositor.renderFrame(viewport);
  }
  auto end = Clock::now();

  const auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  const double avgMs = static_cast<double>(totalUs) / kIterations / 1000.0;

  std::cerr << "[PERF] DragFrameOverhead_10kNodes: " << avgMs << " ms/frame (avg over "
            << kIterations << " iterations, mock renderer)\n";

  // With mock renderer, compositor overhead for 10k nodes should still be well under 16.67ms.
  // The 16.67ms budget is for a FULL frame including rasterization; compositor overhead alone
  // should be a small fraction.
  EXPECT_LT(avgMs, 5.0) << "Compositor overhead per frame exceeds 5ms (mock renderer, 10k nodes)";
}

// Measure click-to-first-drag-update latency — the cold path from "user selects
// an entity" through "pre-warm the layer" to "first composited frame with the
// drag delta applied." This is the user-visible latency the design doc calls
// out in Goal 6 (p50 < 16 ms, p99 < 33 ms on 10k-node scene).
//
// Reports three numbers:
//   1. Prewarm cost — time to rasterize the selected entity's layer for the
//      first time (from Selection hint publish through one renderFrame).
//   2. First drag frame cost — time from drag start (ActiveDrag hint publish
//      + first transform applied) through one renderFrame.
//   3. Combined click-to-first-drag — end-to-end cold path.
//
// Budgets are loose: this is a measurement benchmark, not a regression gate.
// The tight steady-state assertions live in `DragFrameOverhead_*`.
TEST_F(CompositorPerfTest, ClickToFirstDragUpdate_10kNodes) {
  const std::string svg = generateGridSvg(10000, 100);
  SVGDocument document = makeDocument(svg);

  configureMockForCaching();
  CompositorController compositor(document, renderer_);

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  RenderViewport viewport;
  viewport.size = Vector2d(1000, 1000);
  viewport.devicePixelRatio = 1.0;

  // Phase 1: selection published, layer pre-warmed.
  auto prewarmStart = Clock::now();
  ASSERT_TRUE(compositor.promoteEntity(entity, InteractionHint::Selection));
  compositor.renderFrame(viewport);
  auto prewarmEnd = Clock::now();

  // Phase 2: drag begins. Transform applied, first drag frame rendered.
  auto dragStart = Clock::now();
  compositor.setLayerCompositionTransform(entity, Transform2d::Translate(Vector2d(5.0, 5.0)));
  compositor.renderFrame(viewport);
  auto dragEnd = Clock::now();

  const auto prewarmUs =
      std::chrono::duration_cast<std::chrono::microseconds>(prewarmEnd - prewarmStart).count();
  const auto dragUs =
      std::chrono::duration_cast<std::chrono::microseconds>(dragEnd - dragStart).count();
  const double prewarmMs = static_cast<double>(prewarmUs) / 1000.0;
  const double dragMs = static_cast<double>(dragUs) / 1000.0;
  const double combinedMs = prewarmMs + dragMs;

  std::cerr << "[PERF] ClickToFirstDragUpdate_10kNodes: prewarm=" << prewarmMs
            << " ms, first-drag-frame=" << dragMs << " ms, combined=" << combinedMs
            << " ms (mock renderer)\n";

  // Loose budgets — these are baselines, not tight gates. Real rasterization
  // time lands on top of these numbers; the design-doc 16/33 ms budgets assume
  // the compositor overhead is a small fraction of the full-frame cost.
  EXPECT_LT(dragMs, 100.0) << "First drag frame absurdly slow (mock renderer, 10k nodes)";
  EXPECT_LT(combinedMs, 200.0) << "Click-to-first-drag-update absurdly slow";
}

// Click-to-first-drag on a smaller scene — the common editor case.
TEST_F(CompositorPerfTest, ClickToFirstDragUpdate_1kNodes) {
  const std::string svg = generateGridSvg(1000, 32);
  SVGDocument document = makeDocument(svg);

  configureMockForCaching();
  CompositorController compositor(document, renderer_);

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  RenderViewport viewport;
  viewport.size = Vector2d(1000, 1000);
  viewport.devicePixelRatio = 1.0;

  auto prewarmStart = Clock::now();
  ASSERT_TRUE(compositor.promoteEntity(entity, InteractionHint::Selection));
  compositor.renderFrame(viewport);
  auto prewarmEnd = Clock::now();

  auto dragStart = Clock::now();
  compositor.setLayerCompositionTransform(entity, Transform2d::Translate(Vector2d(5.0, 5.0)));
  compositor.renderFrame(viewport);
  auto dragEnd = Clock::now();

  const auto prewarmUs =
      std::chrono::duration_cast<std::chrono::microseconds>(prewarmEnd - prewarmStart).count();
  const auto dragUs =
      std::chrono::duration_cast<std::chrono::microseconds>(dragEnd - dragStart).count();
  const double prewarmMs = static_cast<double>(prewarmUs) / 1000.0;
  const double dragMs = static_cast<double>(dragUs) / 1000.0;
  const double combinedMs = prewarmMs + dragMs;

  std::cerr << "[PERF] ClickToFirstDragUpdate_1kNodes: prewarm=" << prewarmMs
            << " ms, first-drag-frame=" << dragMs << " ms, combined=" << combinedMs
            << " ms (mock renderer)\n";

  EXPECT_LT(dragMs, 50.0) << "First drag frame absurdly slow (mock renderer, 1k nodes)";
  EXPECT_LT(combinedMs, 100.0) << "Click-to-first-drag-update absurdly slow (1k nodes)";
}

// Goal 8 baseline: bucketer reconcile on a 10k-node scene should run in under
// 5% of the parse + ECS-build wall clock. We approximate: parse/prep takes
// ~tens-of-ms on 10k nodes; the bucketer should be well under that.
//
// Reports the reconcile time so future regressions are visible. Loose assertion
// (<100 ms) is a sanity floor — this is a measurement, not a gate.
TEST_F(CompositorPerfTest, ComplexityBucketerReconcile_10kNodes) {
  const std::string svg = generateGridSvg(10000, 100);
  SVGDocument document = makeDocument(svg);

  // Prepare the document so `RenderingInstanceComponent` is populated (the
  // bucketer walks this view).
  ParseWarningSink warningSink;
  RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false, warningSink);

  ComplexityBucketer bucketer;

  // Warm-up run.
  bucketer.reconcile(document.registry());

  // Measure.
  constexpr int kIterations = 10;
  auto start = Clock::now();
  for (int i = 0; i < kIterations; ++i) {
    bucketer.reconcile(document.registry());
  }
  auto end = Clock::now();

  const auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  const double avgMs = static_cast<double>(totalUs) / kIterations / 1000.0;

  std::cerr << "[PERF] ComplexityBucketerReconcile_10kNodes: " << avgMs
            << " ms/pass (avg over " << kIterations << " warm reconciles, "
            << bucketer.stats().candidatesConsidered << " candidates)\n";

  EXPECT_LT(avgMs, 100.0) << "Bucketer reconcile absurdly slow on 10k-node scene";
}

}  // namespace donner::svg::compositor
