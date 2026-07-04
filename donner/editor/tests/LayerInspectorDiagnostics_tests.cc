#include "donner/editor/LayerInspectorDiagnostics.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
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
  EXPECT_EQ(CanvasFreshnessStatusSuffix(static_cast<CanvasFreshness>(255)), "");
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

TEST(LayerInspectorDiagnosticsTest, SerializesPromotedLayersAsCachedLayerTiles) {
  CompositeTileSnapshot immediateLayer;
  immediateLayer.kind = CompositeTileSnapshot::Kind::Layer;
  immediateLayer.id = "layer:99";
  immediateLayer.label = "promoted layer";
  immediateLayer.lastRasterizeMs = 5.0;
  immediateLayer.immediate = true;
  immediateLayer.dynamicHeuristicImmediate = true;
  immediateLayer.immediateBudgetMs = 2.083;
  immediateLayer.immediateBudgetChargeMs = 5.0;
  immediateLayer.estimatedDrawOps = 1;
  immediateLayer.visible = true;

  CompositorHeuristicTelemetryContext context;
  context.state.canvasSize = Vector2i(256, 256);
  context.renderStats.immediateRasterizeMs = 5.0;
  context.renderStats.immediateTileCount = 1;

  const std::vector<CompositeTileSnapshot> tiles = {immediateLayer};
  const std::string json = BuildCompositorHeuristicTelemetryJson(tiles, context);

  EXPECT_NE(json.find("\"layers\":1,\"immediate\":0,\"cached\":1"), std::string::npos);
  EXPECT_NE(json.find("\"kind\":\"layer\""), std::string::npos);
  EXPECT_NE(json.find("\"mode\":\"cached\""), std::string::npos);
  EXPECT_NE(json.find("\"signal\":\"normal\""), std::string::npos);
}

TEST(LayerInspectorDiagnosticsTest, SerializesTileKindsReasonsSignalsAndNonFiniteNumbers) {
  CompositeTileSnapshot background;
  background.kind = CompositeTileSnapshot::Kind::Background;
  background.id = "background";
  background.lastRasterizeMs = std::numeric_limits<double>::infinity();

  CompositeTileSnapshot foreground;
  foreground.kind = CompositeTileSnapshot::Kind::Foreground;
  foreground.id = "foreground";

  CompositeTileSnapshot staticImmediate;
  staticImmediate.kind = CompositeTileSnapshot::Kind::Segment;
  staticImmediate.id = "static";
  staticImmediate.immediate = true;
  staticImmediate.staticHeuristicImmediate = true;

  CompositeTileSnapshot expensive;
  expensive.kind = CompositeTileSnapshot::Kind::Segment;
  expensive.id = "expensive";
  expensive.hasExpensiveEffect = true;

  CompositeTileSnapshot invisible;
  invisible.kind = CompositeTileSnapshot::Kind::Segment;
  invisible.id = "invisible";
  invisible.visible = false;
  invisible.estimatedDrawOps = 3;

  CompositeTileSnapshot cachedFast;
  cachedFast.kind = CompositeTileSnapshot::Kind::Segment;
  cachedFast.id = "cached-fast";
  cachedFast.visible = true;
  cachedFast.estimatedDrawOps = 5;
  cachedFast.immediateBudgetMs = 2.0;
  cachedFast.lastRasterizeMs = 1.0;

  CompositorHeuristicTelemetryContext context;
  context.viewportZoom = std::numeric_limits<double>::quiet_NaN();
  context.viewportDesiredCanvas = Vector2i(200, 100);
  context.documentCanvas = Vector2i(100, 100);
  context.state.canvasSize = Vector2i(100, 100);

  const std::vector<CompositeTileSnapshot> tiles = {background, foreground, staticImmediate,
                                                    expensive,  invisible,  cachedFast};
  const std::string json = BuildCompositorHeuristicTelemetryJson(tiles, context);

  EXPECT_NE(json.find("\"freshness\":\"commit_stalled\""), std::string::npos);
  EXPECT_NE(json.find("\"zoom\":null"), std::string::npos);
  EXPECT_NE(json.find("\"id\":\"background\",\"kind\":\"background\""), std::string::npos);
  EXPECT_NE(json.find("\"id\":\"foreground\",\"kind\":\"foreground\""), std::string::npos);
  EXPECT_NE(json.find("\"reason\":\"static_heuristic\""), std::string::npos);
  EXPECT_NE(json.find("\"reason\":\"expensive_effect\""), std::string::npos);
  EXPECT_NE(json.find("\"reason\":\"not_visible\""), std::string::npos);
  EXPECT_NE(json.find("\"signal\":\"cached_fast_candidate\""), std::string::npos);
  EXPECT_NE(json.find("\"last_ms\":null"), std::string::npos);
}

TEST(LayerInspectorDiagnosticsTest, EscapesJsonStringsAndSerializesUnknownTileKind) {
  CompositeTileSnapshot unknown;
  unknown.kind = static_cast<CompositeTileSnapshot::Kind>(255);
  unknown.id = "quote\"slash\\\b\f\n\r\t";
  unknown.id.push_back(static_cast<char>(0x01));
  unknown.label = "label\nwith\tcontrols";
  unknown.spanRangeLabel = "span\\range\"quoted";
  unknown.immediateBudgetMs = 1.0;
  unknown.lastRasterizeMs = 2.0;
  unknown.visible = true;
  unknown.estimatedDrawOps = 3;

  CompositorHeuristicTelemetryContext context;
  context.state.canvasSize = Vector2i(64, 64);

  const std::vector<CompositeTileSnapshot> tiles = {unknown};
  const std::string json = BuildCompositorHeuristicTelemetryJson(tiles, context);

  EXPECT_NE(json.find(R"("id":"quote\"slash\\\b\f\n\r\t\u0001")"), std::string::npos);
  EXPECT_NE(json.find(R"("kind":"unknown")"), std::string::npos);
  EXPECT_NE(json.find(R"("label":"label\nwith\tcontrols")"), std::string::npos);
  EXPECT_NE(json.find(R"("span":"span\\range\"quoted")"), std::string::npos);
  EXPECT_NE(json.find(R"("mode":"cached")"), std::string::npos);
  EXPECT_NE(json.find(R"("reason":"cached")"), std::string::npos);
  EXPECT_NE(json.find(R"("signal":"normal")"), std::string::npos);
  EXPECT_NE(json.find(R"("over_budget":true)"), std::string::npos);
  EXPECT_NE(json.find(R"("over_budget_cached":0)"), std::string::npos);
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

TEST(LayerInspectorDiagnosticsTest, ReportsTelemetryWriteErrors) {
  std::string error;
  EXPECT_FALSE(AppendCompositorHeuristicTelemetry("", "{}", &error));
  EXPECT_EQ(error, "telemetry path is empty");

  EXPECT_FALSE(SaveCompositorHeuristicTelemetry("", "{}", &error));
  EXPECT_EQ(error, "telemetry path is empty");

  const std::filesystem::path directory =
      std::filesystem::path(::testing::TempDir()) / "donner-telemetry-directory";
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  ASSERT_FALSE(ec);

  EXPECT_FALSE(AppendCompositorHeuristicTelemetry(directory.string(), "{}", &error));
  EXPECT_NE(error.find("failed to open telemetry path:"), std::string::npos);

  EXPECT_FALSE(SaveCompositorHeuristicTelemetry(directory.string(), "{}", &error));
  EXPECT_NE(error.find("failed to open telemetry path:"), std::string::npos);

  EXPECT_FALSE(AppendCompositorHeuristicTelemetry("", "{}", nullptr));
  EXPECT_FALSE(SaveCompositorHeuristicTelemetry("", "{}", nullptr));

  std::filesystem::remove_all(directory, ec);
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
