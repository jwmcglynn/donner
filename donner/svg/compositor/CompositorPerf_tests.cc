#include "donner/svg/compositor/CompositorController.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <sstream>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGraphicsElement.h"
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
    target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(i * 1.0, i * 0.5));
    compositor.renderFrame(viewport);
  }

  // Measure.
  constexpr int kIterations = 100;
  auto start = Clock::now();
  for (int i = 0; i < kIterations; ++i) {
    target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(i * 2.0, i * 1.0));
    compositor.renderFrame(viewport);
  }
  auto end = Clock::now();

  const auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  const double avgMs = static_cast<double>(totalUs) / kIterations / 1000.0;

  // Report.
  std::cerr << "[PERF] DragFrameOverhead_1kNodes: " << avgMs << " ms/frame (avg over "
            << kIterations << " iterations, mock renderer)\n";

  // Ceiling is a CI-runner-shape absurdity gate, not an aspirational target.
  // GitHub's shared `ubuntu-latest` runners land this test around 11-15 ms
  // (mock renderer, 1k nodes) — tight-bound rasterize walks every segment
  // once per frame even when no segment is dirty. 30 ms = 2-3x observed, so
  // the gate still catches a real 5x+ regression but doesn't flake on a busy
  // runner.
  EXPECT_LT(avgMs, 30.0) << "Compositor overhead per frame exceeds 30ms (mock renderer, 1k nodes)";
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
    target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(i * 1.0, i * 0.5));
    compositor.renderFrame(viewport);
  }

  // Measure.
  constexpr int kIterations = 50;
  auto start = Clock::now();
  for (int i = 0; i < kIterations; ++i) {
    target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(i * 2.0, i * 1.0));
    compositor.renderFrame(viewport);
  }
  auto end = Clock::now();

  const auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  const double avgMs = static_cast<double>(totalUs) / kIterations / 1000.0;

  std::cerr << "[PERF] DragFrameOverhead_10kNodes: " << avgMs << " ms/frame (avg over "
            << kIterations << " iterations, mock renderer)\n";

  // CI-runner absurdity gate. GitHub `ubuntu-latest` lands around 60-135 ms
  // per frame with 10k nodes under a mock renderer — the per-segment dirty
  // walk is still O(entities) even when nothing needs rasterizing. 350 ms
  // = 2.5-3x observed, which still catches a real regression (e.g. full
  // re-rasterize every frame would be seconds) but tolerates runner load.
  EXPECT_LT(avgMs, 350.0) << "Compositor overhead per frame exceeds 350ms (mock renderer, 10k nodes)";
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

  // Phase 2: drag begins. Transform applied to the DOM, first drag frame rendered.
  auto dragStart = Clock::now();
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(5.0, 5.0)));
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

  // Loose budgets — these are absurdity gates, not tight perf targets. The
  // dominant cost at 10k nodes is the first-ever `instantiateRenderTree`
  // (style cascade over every RIC) plus the N+1 static-segment traversals
  // — both linear in document size. On current hardware that lands around
  // 800-900 ms on the mock renderer; the budget here is "did something
  // blow up by 2x?", not "is compositor overhead negligible?".
  //
  // The first-drag-frame budget is what actually matters for interactive
  // feel: once prewarm has cached the layer + segments, every subsequent
  // drag frame reuses them via the translation fast-path. That's the
  // number that directly determines "does dragging a letter feel smooth?"
  // (Phase B per-segment dirty tracking keeps this fast even when
  // non-promoted entities mutate.)
  // CI-runner shape: shared runners land dragMs around 110-135 ms and
  // combinedMs around 1550-2120 ms. Budgets set to ~2.5x observed to catch
  // real regressions without flaking.
  EXPECT_LT(dragMs, 300.0) << "First drag frame absurdly slow (mock renderer, 10k nodes)";
  EXPECT_LT(combinedMs, 4000.0) << "Click-to-first-drag-update absurdly slow";
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
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(5.0, 5.0)));
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

  // CI-runner shape: observed dragMs ~12 ms, combinedMs ~250 ms on shared
  // runners. 2.5x observed for reliable CI.
  EXPECT_LT(dragMs, 100.0) << "First drag frame absurdly slow (mock renderer, 1k nodes)";
  EXPECT_LT(combinedMs, 650.0) << "Click-to-first-drag-update absurdly slow (1k nodes)";
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
