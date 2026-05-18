#include "tools/mcp-servers/editor-control/EditorControlSession.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "donner/editor/repro/ReproFile.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace donner::editor::mcp {
namespace {

using nlohmann::json;

constexpr std::string_view kFilteredScene = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 120 80" width="120" height="80">
  <defs>
    <filter id="blur" x="-20%" y="-20%" width="140%" height="140%">
      <feGaussianBlur in="SourceGraphic" stdDeviation="4"/>
    </filter>
  </defs>
  <rect width="120" height="80" fill="white"/>
  <g id="glow" filter="url(#blur)">
    <circle cx="70" cy="40" r="18" fill="red"/>
  </g>
  <rect id="target" x="20" y="25" width="20" height="20" fill="blue"/>
</svg>
)svg";

std::string TileSignature(const json& tile) {
  std::string result = tile.value("kind", "");
  result += ":";
  result += tile.value("id", "");
  if (tile.value("is_drag_target", false)) {
    result += "*";
  }
  return result;
}

std::string TileGenerationSignature(const json& tile) {
  std::string result = TileSignature(tile);
  result += "#gen";
  result += std::to_string(tile.value("generation", 0));
  return result;
}

std::vector<std::string> PreviewTileSignature(const json& preview) {
  std::vector<std::string> signature;
  if (preview.is_null()) {
    return signature;
  }

  const json& tiles = preview["tiles"];
  signature.reserve(tiles.size());
  for (const json& tile : tiles) {
    signature.push_back(TileSignature(tile));
  }
  return signature;
}

std::vector<std::string> PreviewTileGenerationSignature(const json& preview) {
  std::vector<std::string> signature;
  if (preview.is_null()) {
    return signature;
  }

  const json& tiles = preview["tiles"];
  signature.reserve(tiles.size());
  for (const json& tile : tiles) {
    signature.push_back(TileGenerationSignature(tile));
  }
  return signature;
}

bool HasLeftMouseDownEvent(const json& frame) {
  if (!frame.contains("events")) {
    return false;
  }
  for (const json& event : frame["events"]) {
    if (event.value("kind", "") == "mdown" && event.value("mouse_button", -1) == 0) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> StageTileSignature(const json& stage, std::string_view previewKey) {
  const std::string key(previewKey);
  if (!stage.contains(key) || stage[key].is_null()) {
    return {};
  }

  return PreviewTileSignature(stage[key]);
}

std::vector<std::vector<std::string>> DragTileSignatures(const ToolCallResult& drag,
                                                         std::string_view previewKey,
                                                         bool includeRelease,
                                                         bool includeEmptySignatures) {
  std::vector<std::vector<std::string>> signatures;

  const auto collectStages = [&](const json& stages) {
    for (const json& stage : stages) {
      std::vector<std::string> signature = StageTileSignature(stage, previewKey);
      if (!signature.empty() || includeEmptySignatures) {
        signatures.push_back(std::move(signature));
      }
    }
  };

  for (const json& frame : drag.body["frames"]) {
    collectStages(frame["stages"]);
  }
  if (includeRelease && drag.body.contains("release")) {
    collectStages(drag.body["release"]["stages"]);
  }

  return signatures;
}

void ExpectEveryStageHasTileOrder(std::string_view label, const ToolCallResult& drag,
                                  std::string_view previewKey, bool includeRelease,
                                  bool includeEmptySignatures,
                                  const std::vector<std::string>& expected) {
  const std::vector<std::vector<std::string>> signatures =
      DragTileSignatures(drag, previewKey, includeRelease, includeEmptySignatures);
  ASSERT_FALSE(signatures.empty()) << label;
  for (size_t i = 0; i < signatures.size(); ++i) {
    EXPECT_EQ(signatures[i], expected) << label << " stage " << i;
  }
}

void ExpectBeforeRenderDisplayHandoff(
    std::string_view label, const ToolCallResult& drag,
    const std::vector<std::string>& expectedAfterFirstRender,
    const std::optional<std::vector<std::string>>& expectedBeforeFirstRender = std::nullopt) {
  const json& frames = drag.body["frames"];
  ASSERT_GE(frames.size(), 2) << label;

  const json& firstDisplay = frames[0]["display_before_render"];
  if (expectedBeforeFirstRender.has_value()) {
    EXPECT_EQ(firstDisplay.value("path", ""), "tiles") << label;
    EXPECT_EQ(PreviewTileSignature(firstDisplay), *expectedBeforeFirstRender) << label;
  } else {
    EXPECT_EQ(firstDisplay.value("path", ""), "flat") << label;
    EXPECT_TRUE(PreviewTileSignature(firstDisplay).empty()) << label;
  }

  for (size_t i = 1; i < frames.size(); ++i) {
    const json& display = frames[i]["display_before_render"];
    EXPECT_EQ(display.value("path", ""), "tiles") << label << " frame " << i;
    EXPECT_EQ(PreviewTileSignature(display), expectedAfterFirstRender) << label << " frame " << i;
  }
}

TEST(EditorControlSessionTest, ToolListExposesSelectorDragAndRenderTools) {
  const json tools = EditorControlSession::toolList();

  auto hasTool = [&](std::string_view name) {
    for (const json& tool : tools) {
      if (tool.value("name", "") == name) return true;
    }
    return false;
  };

  EXPECT_TRUE(hasTool("select_by_selector"));
  EXPECT_TRUE(hasTool("drag_selector"));
  EXPECT_TRUE(hasTool("render_frame"));
  EXPECT_TRUE(hasTool("start_rnr_recording"));
  EXPECT_TRUE(hasTool("stop_rnr_recording"));
  EXPECT_TRUE(hasTool("replay_rnr"));
}

TEST(EditorControlSessionTest, InvalidToolCallReturnsErrorResult) {
  EditorControlSession session;

  ToolCallResult result = session.handleToolCall("drag_selector", json::object());

  EXPECT_TRUE(result.isError);
  EXPECT_TRUE(result.body["error"].is_string());
}

TEST(EditorControlSessionTest, SelectsBySelectorAndDragsThroughCompositedPreview) {
  EditorControlSession session;

  ToolCallResult load =
      session.handleToolCall("load_svg", json{{"svg_source", std::string(kFilteredScene)},
                                              {"canvas_width", 120},
                                              {"canvas_height", 80},
                                              {"render_after_load", false}});
  ASSERT_TRUE(load.body.value("ok", false));

  ToolCallResult select =
      session.handleToolCall("select_by_selector", json{{"selector", "#target"}, {"render", true}});
  ASSERT_TRUE(select.body.value("ok", false));
  ASSERT_EQ(select.body["selection"].value("id", ""), "target");
  ASSERT_TRUE(select.body.contains("render_stages"));

  ToolCallResult drag =
      session.handleToolCall("drag_selector", json{{"selector", "#target"},
                                                   {"delta_x", 12.0},
                                                   {"delta_y", 3.0},
                                                   {"frames", 3},
                                                   {"include_final_frame", true}});
  ASSERT_TRUE(drag.body.value("ok", false));
  EXPECT_EQ(drag.body["final_selection"].value("id", ""), "target");
  EXPECT_FALSE(drag.images.empty());

  const json& firstMoveStages = drag.body["frames"][1]["stages"];
  ASSERT_FALSE(firstMoveStages.empty());
  bool sawCompositedPreview = false;
  for (const json& stage : firstMoveStages) {
    if (!stage["composited_preview"].is_null()) {
      sawCompositedPreview = true;
      EXPECT_GT(stage["composited_preview"].value("tile_count", 0), 0);
      for (const json& tile : stage["composited_preview"]["tiles"]) {
        ASSERT_TRUE(tile["bitmap"].contains("content_hash"));
        if (!tile["bitmap"].value("empty", true)) {
          EXPECT_TRUE(tile["bitmap"]["content_hash"].is_string());
        }
      }
      for (const json& tile : stage["display_preview"]["tiles"]) {
        EXPECT_TRUE(tile.contains("content_hash"));
        EXPECT_TRUE(tile["content_hash"].is_string());
      }
    }
  }
  EXPECT_TRUE(sawCompositedPreview);
}

TEST(EditorControlSessionTest, SplashOThenRDragKeepsStableSplitLayerPaintOrder) {
  std::ifstream splash("donner_splash.svg", std::ios::binary);
  if (!splash.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ostringstream buffer;
  buffer << splash.rdbuf();

  EditorControlSession session;
  ASSERT_TRUE(session
                  .handleToolCall("load_svg", json{{"svg_source", buffer.str()},
                                                   {"canvas_width", 892},
                                                   {"canvas_height", 512},
                                                   {"render_after_load", true}})
                  .body.value("ok", false));

  ToolCallResult oDrag =
      session.handleToolCall("drag_selector", json{{"selector", "#Donner path.cls-82"},
                                                   {"delta_x", 8.0},
                                                   {"frames", 4},
                                                   {"include_final_frame", false}});
  ASSERT_TRUE(oDrag.body.value("ok", false));

  ToolCallResult rDrag =
      session.handleToolCall("drag_selector", json{{"selector", "#Donner path.cls-86"},
                                                   {"delta_x", 8.0},
                                                   {"frames", 4},
                                                   {"include_final_frame", false}});
  ASSERT_TRUE(rDrag.body.value("ok", false));

  const std::vector<std::string> expectedODragOrder = {
      "segment:640",           "layer:9223372036854776448",
      "segment:2748779070096", "layer:9223372036854776464*",
      "segment:2817498546848", "layer:9223372036854776480",
      "segment:2886218023589", "layer:9223372036854776485",
      "segment:2907692860200", "layer:9223372036854776616",
      "segment:3470333575978", "layer:9223372036854776618",
      "segment:3478923510575", "layer:9223372036854776623",
      "segment:3500398347071", "layer:9223372036854776639",
      "segment:3569117822976",
  };
  const std::vector<std::string> expectedRDragOrder = {
      "segment:640",           "layer:9223372036854776448",
      "segment:2748779070096", "layer:9223372036854776464",
      "segment:2817498546840", "layer:9223372036854776472*",
      "segment:2851858285216", "layer:9223372036854776480",
      "segment:2886218023589", "layer:9223372036854776485",
      "segment:2907692860200", "layer:9223372036854776616",
      "segment:3470333575978", "layer:9223372036854776618",
      "segment:3478923510575", "layer:9223372036854776623",
      "segment:3500398347071", "layer:9223372036854776639",
      "segment:3569117822976",
  };

  ExpectEveryStageHasTileOrder("O drag worker", oDrag, "composited_preview",
                               /*includeRelease=*/true, /*includeEmptySignatures=*/false,
                               expectedODragOrder);
  ExpectEveryStageHasTileOrder("R drag after O drag worker", rDrag, "composited_preview",
                               /*includeRelease=*/true, /*includeEmptySignatures=*/false,
                               expectedRDragOrder);
  ExpectEveryStageHasTileOrder("O drag displayed", oDrag, "display_preview",
                               /*includeRelease=*/false, /*includeEmptySignatures=*/true,
                               expectedODragOrder);
  ExpectEveryStageHasTileOrder("R drag after O drag displayed", rDrag, "display_preview",
                               /*includeRelease=*/false, /*includeEmptySignatures=*/true,
                               expectedRDragOrder);
  ExpectBeforeRenderDisplayHandoff("O drag before render", oDrag, expectedODragOrder);
  ExpectBeforeRenderDisplayHandoff("R drag after O drag before render", rDrag, expectedRDragOrder);
}

TEST(EditorControlSessionTest,
     ReplayFilteredElementFlashAfterDragsDoesNotDisplayPreviousDragTargetOnSecondClick) {
  EditorControlSession session;
  ToolCallResult replay = session.handleToolCall(
      "replay_rnr",
      json{{"rnr_path", "donner/editor/tests/filtered-element-flash-after-drags-2.rnr"},
           {"include_frame_results", true},
           {"include_display_diff", true},
           {"max_frame_results", 200},
           {"include_final_frame", false}});
  ASSERT_TRUE(replay.body.value("ok", false)) << replay.body.dump(2);

  const json& frames = replay.body["frames"];
  ASSERT_FALSE(frames.empty());

  const json* secondMouseDownFrame = nullptr;
  int mouseDownCount = 0;
  for (const json& frame : frames) {
    if (!HasLeftMouseDownEvent(frame)) {
      continue;
    }
    ++mouseDownCount;
    if (mouseDownCount == 2) {
      secondMouseDownFrame = &frame;
      break;
    }
  }

  ASSERT_NE(secondMouseDownFrame, nullptr)
      << "Replay did not store the second left-button mouse-down frame. Stored frames: "
      << frames.dump(2);

  const json& display = (*secondMouseDownFrame)["display_before_render"];
  ASSERT_TRUE(display["active_drag_preview"].is_object()) << secondMouseDownFrame->dump(2);

  const std::uint32_t activeEntity =
      display["active_drag_preview"].value("entity", static_cast<std::uint32_t>(0));
  ASSERT_NE(activeEntity, 0u) << secondMouseDownFrame->dump(2);

  const std::string displayPath = display.value("path", "");
  ASSERT_TRUE(displayPath == "flat" || displayPath == "tiles")
      << "Unexpected second-click display path: " << displayPath << "\n"
      << secondMouseDownFrame->dump(2);

  const json& diff = (*secondMouseDownFrame)["display_before_render_diff_from_final"];
  ASSERT_TRUE(diff.value("available", false)) << secondMouseDownFrame->dump(2);
  EXPECT_EQ(diff.value("differing_pixels", -1), 0)
      << "The first display handoff after the second click must match the just-rendered final "
         "frame. Frame "
      << (*secondMouseDownFrame).value("frame_index", 0) << ", display path=" << displayPath
      << ", max channel delta=" << diff.value("max_channel_delta", -1)
      << ", artifacts=" << diff.value("artifacts", json::object()).dump()
      << ", tile generations=" << json(PreviewTileGenerationSignature(display)).dump();
  EXPECT_EQ(diff.value("max_channel_delta", -1), 0)
      << "The first display handoff after the second click must be pixel-identical to final.";

  if (displayPath == "tiles") {
    const std::uint32_t displayedEntity =
        display.value("displayed_entity", static_cast<std::uint32_t>(0));
    EXPECT_EQ(displayedEntity, activeEntity)
        << "The first display handoff after the second click is drawing the previous drag "
           "target's cached composited tiles. This is the operator-visible flash: the R click has "
           "an active drag preview for entity "
        << activeEntity << ", but the display path is still blitting cached tiles for entity "
        << displayedEntity << ". Frame " << (*secondMouseDownFrame).value("frame_index", 0)
        << ", display-vs-final differing pixels=" << diff.value("differing_pixels", -1)
        << ", max channel delta=" << diff.value("max_channel_delta", -1)
        << ", tile generations=" << json(PreviewTileGenerationSignature(display)).dump();
  }
}

TEST(EditorControlSessionTest,
     ReplayFilteredElementFlashAfterDragsGuiScheduleDoesNotFlashOnSecondClick) {
  EditorControlSession session;
  ToolCallResult replay = session.handleToolCall(
      "replay_rnr",
      json{{"rnr_path", "donner/editor/tests/filtered-element-flash-after-drags-2.rnr"},
           {"simulate_editor_shell_frame_loop", true},
           {"include_frame_results", true},
           {"include_display_diff", true},
           {"compare_presented_after_left_mouse_down", 2},
           {"max_frame_results", 240},
           {"include_final_frame", false}});
  ASSERT_TRUE(replay.body.value("ok", false)) << replay.body.dump(2);

  const json& frames = replay.body["frames"];
  ASSERT_FALSE(frames.empty());

  const json* secondMouseDownFrame = nullptr;
  for (const json& frame : frames) {
    if (frame.value("left_mouse_down_ordinal", 0) == 2) {
      secondMouseDownFrame = &frame;
      break;
    }
  }

  ASSERT_NE(secondMouseDownFrame, nullptr)
      << "GUI-scheduled replay did not store the second left-button mouse-down frame. Stored "
         "frames: "
      << frames.dump(2);
  EXPECT_GE((*secondMouseDownFrame).value("frame_index", 0), 150);
  EXPECT_LE((*secondMouseDownFrame).value("frame_index", 0), 156);

  const json& presented = (*secondMouseDownFrame)["presented_frame"];
  ASSERT_TRUE(presented["active_drag_preview"].is_object()) << secondMouseDownFrame->dump(2);
  const std::uint32_t activeEntity =
      presented["active_drag_preview"].value("entity", static_cast<std::uint32_t>(0));
  ASSERT_NE(activeEntity, 0u) << secondMouseDownFrame->dump(2);

  if (presented.value("path", "") == "tiles") {
    const std::uint32_t displayedEntity =
        presented.value("displayed_entity", static_cast<std::uint32_t>(0));
    EXPECT_EQ(displayedEntity, activeEntity)
        << "The GUI-scheduled first presented frame after the second click is blitting cached "
           "tiles for the wrong entity. Frame "
        << (*secondMouseDownFrame).value("frame_index", 0)
        << ", tile generations=" << json(PreviewTileGenerationSignature(presented)).dump();
  }

  const json& diff = (*secondMouseDownFrame)["presented_frame_diff_from_eventual_final"];
  ASSERT_TRUE(diff.value("available", false)) << secondMouseDownFrame->dump(2);
  EXPECT_EQ(diff.value("differing_pixels", -1), 0)
      << "The GUI-scheduled first presented frame after the second click differs from the "
         "eventual final render. Frame "
      << (*secondMouseDownFrame).value("frame_index", 0)
      << ", display path=" << presented.value("path", "")
      << ", max channel delta=" << diff.value("max_channel_delta", -1)
      << ", artifacts=" << diff.value("artifacts", json::object()).dump()
      << ", tile generations=" << json(PreviewTileGenerationSignature(presented)).dump();
  EXPECT_EQ(diff.value("max_channel_delta", -1), 0)
      << "The GUI-scheduled first presented frame after the second click must be "
         "pixel-identical to the eventual final render.";
}

TEST(EditorControlSessionTest, RecordsAndReplaysRnrFromSelectorDrag) {
  const std::filesystem::path tempDir = std::filesystem::temp_directory_path();
  const std::filesystem::path svgPath = tempDir / "donner_editor_control_rnr_roundtrip.svg";
  const std::filesystem::path rnrPath = tempDir / "donner_editor_control_rnr_roundtrip.rnr";
  {
    std::ofstream svg(svgPath, std::ios::binary);
    ASSERT_TRUE(svg.is_open());
    svg << kFilteredScene;
  }

  EditorControlSession session;
  ToolCallResult load =
      session.handleToolCall("load_svg", json{{"svg_source", std::string(kFilteredScene)},
                                              {"source_path", svgPath.string()},
                                              {"canvas_width", 120},
                                              {"canvas_height", 80},
                                              {"render_after_load", false}});
  ASSERT_TRUE(load.body.value("ok", false));

  ToolCallResult start = session.handleToolCall(
      "start_rnr_recording",
      json{{"output_path", rnrPath.string()}, {"svg_path", svgPath.string()}});
  ASSERT_TRUE(start.body.value("ok", false));

  ToolCallResult drag =
      session.handleToolCall("drag_selector", json{{"selector", "#target"},
                                                   {"delta_x", 12.0},
                                                   {"delta_y", 3.0},
                                                   {"frames", 2},
                                                   {"include_final_frame", false}});
  ASSERT_TRUE(drag.body.value("ok", false));

  ToolCallResult stop = session.handleToolCall("stop_rnr_recording", json::object());
  ASSERT_TRUE(stop.body.value("ok", false));
  EXPECT_EQ(stop.body.value("frame_count", 0), 4);

  std::optional<repro::ReproFile> recorded = repro::ReadReproFile(rnrPath);
  ASSERT_TRUE(recorded.has_value());
  ASSERT_EQ(recorded->frames.size(), 4u);
  ASSERT_EQ(recorded->frames.front().events.size(), 1u);
  EXPECT_EQ(recorded->frames.front().events.front().kind, repro::ReproEvent::Kind::MouseDown);
  ASSERT_EQ(recorded->frames.back().events.size(), 1u);
  EXPECT_EQ(recorded->frames.back().events.front().kind, repro::ReproEvent::Kind::MouseUp);

  const std::filesystem::path idleRnrPath =
      tempDir / "donner_editor_control_rnr_roundtrip_with_idle.rnr";
  repro::ReproFile recordedWithIdle = *recorded;
  ASSERT_FALSE(recordedWithIdle.frames.empty());
  repro::ReproFrame idleFrame = recordedWithIdle.frames.front();
  idleFrame.index = 1000;
  idleFrame.mouseButtonMask = 0;
  idleFrame.events.clear();
  recordedWithIdle.frames.insert(recordedWithIdle.frames.begin(), 2, idleFrame);
  ASSERT_TRUE(repro::WriteReproFile(idleRnrPath, recordedWithIdle));

  ToolCallResult replay =
      session.handleToolCall("replay_rnr", json{{"rnr_path", idleRnrPath.string()},
                                                {"include_frame_results", true},
                                                {"include_display_diff", true},
                                                {"max_frame_results", 10},
                                                {"include_final_frame", false}});
  ASSERT_TRUE(replay.body.value("ok", false));
  EXPECT_EQ(replay.body.value("mouse_up_count", 0), 1);
  EXPECT_EQ(replay.body.value("processed_frame_count", 0), 4);
  EXPECT_EQ(replay.body.value("skipped_idle_frame_count", 0), 2);
  EXPECT_GT(replay.body.value("rendered_frame_count", 0), 0);
  ASSERT_TRUE(replay.body["final_selection"].is_object());
  EXPECT_EQ(replay.body["final_selection"].value("id", ""), "target");
  ASSERT_FALSE(replay.body["frames"].empty());
  EXPECT_TRUE(replay.body["frames"].front().contains("display_before_render"));
  EXPECT_TRUE(replay.body["frames"].front().contains("display_before_render_diff_from_final"));
  bool sawActiveTileDisplay = false;
  for (const json& frame : replay.body["frames"]) {
    const json& beforeRender = frame["display_before_render"];
    if (beforeRender.value("path", "") == "tiles" &&
        beforeRender.value("has_active_drag_preview", false)) {
      sawActiveTileDisplay = true;
      const json& diff = frame["display_before_render_diff_from_final"];
      ASSERT_TRUE(diff.value("available", false));
      const json& previewTranslation = beforeRender["displayed_drag_preview"]["translation_doc"];
      for (const json& tile : beforeRender["tiles"]) {
        if (!tile.value("is_drag_target", false)) {
          continue;
        }
        EXPECT_EQ(tile["effective_drag_translation_doc"], previewTranslation)
            << "active drags should draw cached drag-target tiles at the live preview translation";
      }
    }
    for (const json& stage : frame["stages"]) {
      if (stage["composited_preview"].is_null()) {
        continue;
      }
      for (const json& tile : stage["composited_preview"]["tiles"]) {
        EXPECT_TRUE(tile["bitmap"].contains("content_hash"));
      }
    }
  }
  EXPECT_TRUE(sawActiveTileDisplay);
}

}  // namespace
}  // namespace donner::editor::mcp
