#include "donner/editor/LayerInspectorDiagnostics.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace donner::editor {
namespace {

using CompositeTileSnapshot = svg::compositor::CompositorController::CompositeTileSnapshot;

TEST(LayerInspectorDiagnosticsTest, ClassifiesCurrentCanvas) {
  const Vector2i canvas(320, 240);

  EXPECT_EQ(ClassifyCanvasFreshness(canvas, canvas, canvas), CanvasFreshness::Current);
  EXPECT_EQ(CanvasFreshnessStatusSuffix(CanvasFreshness::Current), "");
}

TEST(LayerInspectorDiagnosticsTest, CommitStallTakesPrecedence) {
  EXPECT_EQ(ClassifyCanvasFreshness(Vector2i(640, 480), Vector2i(320, 240), Vector2i(160, 120)),
            CanvasFreshness::CommitStalled);
  EXPECT_EQ(CanvasFreshnessStatusSuffix(CanvasFreshness::CommitStalled),
            "  \u2190 commit stalled vs desired");
}

TEST(LayerInspectorDiagnosticsTest, DetectsCompositorBehindDocumentCanvas) {
  EXPECT_EQ(ClassifyCanvasFreshness(Vector2i(640, 480), Vector2i(640, 480), Vector2i(320, 240)),
            CanvasFreshness::CompositorBehind);
  EXPECT_EQ(CanvasFreshnessStatusSuffix(CanvasFreshness::CompositorBehind),
            "  \u2190 compositor not yet re-rasterized");
}

TEST(LayerInspectorDiagnosticsTest, SerializesHeuristicTelemetryForOfflineRefinement) {
  CompositeTileSnapshot immediateTile;
  immediateTile.kind = CompositeTileSnapshot::Kind::Segment;
  immediateTile.id = "seg:1";
  immediateTile.label = "segment 1";
  immediateTile.spanRangeLabel = "rect#Background_shine #42";
  immediateTile.bitmapDims = Vector2i(128, 64);
  immediateTile.generation = 12;
  immediateTile.lastRasterizeMs = 10.5;
  immediateTile.immediate = true;
  immediateTile.dynamicHeuristicImmediate = true;
  immediateTile.immediateBudgetMs = 2.083;
  immediateTile.immediateBudgetChargeMs = 10.5;
  immediateTile.estimatedDrawOps = 7;
  immediateTile.estimatedPathVerbs = 84;
  immediateTile.visible = true;
  immediateTile.boundsCanvas = Box2d(Vector2d(10.0, 20.0), Vector2d(138.0, 84.0));
  immediateTile.estimatedRetainedBytes = 32768;
  immediateTile.estimatedRedrawCost = 123.0;
  immediateTile.estimatedCacheOverheadCost = 456.0;

  CompositeTileSnapshot cachedTile;
  cachedTile.kind = CompositeTileSnapshot::Kind::Segment;
  cachedTile.id = "seg:2";
  cachedTile.label = "segment 2";
  cachedTile.demotedDynamicImmediate = true;
  cachedTile.immediateBudgetMs = 2.083;
  cachedTile.lastRasterizeMs = 4.0;

  const std::vector<CompositeTileSnapshot> tiles = {immediateTile, cachedTile};
  CompositorHeuristicTelemetryContext context;
  context.viewportZoom = 3.0;
  context.viewportDpr = 2.0;
  context.viewportDesiredCanvas = Vector2i(1024, 768);
  context.documentCanvas = Vector2i(1024, 768);
  context.activeTilesViewportBounded = true;
  context.overviewInfillAvailable = true;
  context.activeRasterDocumentRect = Box2d::FromXYWH(10.0, 20.0, 50.0, 40.0);
  context.overviewRasterDocumentRect = Box2d::FromXYWH(0.0, 0.0, 100.0, 80.0);
  context.activeOutputSizePx = Vector2i(512, 384);
  context.overviewOutputSizePx = Vector2i(1024, 768);
  context.state.canvasSize = Vector2i(1024, 768);
  context.fastPath.fastPathFrames = 5;
  context.fastPath.slowPathFramesWithDirty = 1;
  context.renderStats.immediateRasterizeMs = 10.5;
  context.renderStats.cachedRasterizeMs = 4.0;
  context.renderStats.immediateTileCount = 1;
  context.renderStats.cachedTileCount = 1;

  const std::string json = BuildCompositorHeuristicTelemetryJson(tiles, context);

  EXPECT_NE(json.find("\"format\":\"donner-compositor-heuristics-v1\""), std::string::npos);
  EXPECT_NE(json.find("\"rnd_imm_ms\":10.500"), std::string::npos);
  EXPECT_NE(json.find("\"over_budget_immediate\":1"), std::string::npos);
  EXPECT_NE(json.find("\"demoted_dynamic\":1"), std::string::npos);
  EXPECT_NE(json.find("\"span\":\"rect#Background_shine #42\""), std::string::npos);
  EXPECT_NE(json.find("\"reason\":\"dynamic_timing\""), std::string::npos);
  EXPECT_NE(json.find("\"signal\":\"over_budget_immediate\""), std::string::npos);
  EXPECT_NE(json.find("\"bounds\":{\"tl\":[10.000,20.000]"), std::string::npos);
  EXPECT_NE(json.find("\"active_viewport_bounded\":true"), std::string::npos);
  EXPECT_NE(json.find("\"overview_infill\":true"), std::string::npos);
  EXPECT_NE(json.find("\"active_output_canvas\":[512,384]"), std::string::npos);
  EXPECT_NE(json.find("\"overview_raster_rect\":{\"tl\":[0.000,0.000]"), std::string::npos);

  const std::string sampleJson =
      BuildCompositorHeuristicTelemetrySampleJson(immediateTile, context, 17);
  EXPECT_NE(sampleJson.find("\"format\":\"donner-compositor-heuristic-sample-v1\""),
            std::string::npos);
  EXPECT_NE(sampleJson.find("\"seq\":17"), std::string::npos);
  EXPECT_NE(sampleJson.find("\"tile\":{\"id\":\"seg:1\""), std::string::npos);
}

TEST(LayerInspectorDiagnosticsTest, AppendsHeuristicTelemetryJsonLine) {
  const std::string path = ::testing::TempDir() + "/donner-heuristic-telemetry.jsonl";
  std::string error;
  ASSERT_TRUE(AppendCompositorHeuristicTelemetry(path, "{\"ok\":true}\n", &error)) << error;

  std::ifstream input(path);
  ASSERT_TRUE(input.is_open());
  std::string line;
  std::getline(input, line);
  EXPECT_EQ(line, "{\"ok\":true}");
}

TEST(LayerInspectorDiagnosticsTest, SavesHeuristicTelemetryJsonLines) {
  const std::string path = ::testing::TempDir() + "/donner-heuristic-telemetry-save.jsonl";
  std::string error;
  ASSERT_TRUE(SaveCompositorHeuristicTelemetry(path, "{\"a\":1}\n{\"b\":2}\n", &error)) << error;

  std::ifstream input(path);
  ASSERT_TRUE(input.is_open());
  std::string firstLine;
  std::string secondLine;
  std::getline(input, firstLine);
  std::getline(input, secondLine);
  EXPECT_EQ(firstLine, "{\"a\":1}");
  EXPECT_EQ(secondLine, "{\"b\":2}");

  ASSERT_TRUE(SaveCompositorHeuristicTelemetry(path, "{\"c\":3}\n", &error)) << error;
  input.close();
  input.open(path);
  ASSERT_TRUE(input.is_open());
  std::string onlyLine;
  std::getline(input, onlyLine);
  EXPECT_EQ(onlyLine, "{\"c\":3}");
}

}  // namespace
}  // namespace donner::editor
