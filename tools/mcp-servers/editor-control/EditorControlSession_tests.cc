#include "tools/mcp-servers/editor-control/EditorControlSession.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/tests/TestTempDir.h"
#include "donner/editor/repro/ReproFile.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace donner::editor::mcp {
namespace {

// Writable scratch directory for tests that round-trip a file. Prefer bazel's
// `TEST_TMPDIR` (always writable, including under sandboxed / remote execution)
// over `std::filesystem::temp_directory_path()`, which resolves to a read-only
// `/tmp` on remote-execution workers and fails the file rename.
std::filesystem::path TestScratchDir() {
  if (const char* testTmpDir = std::getenv("TEST_TMPDIR");
      testTmpDir != nullptr && testTmpDir[0] != '\0') {
    return std::filesystem::path(testTmpDir);
  }
  return std::filesystem::temp_directory_path();
}

using nlohmann::json;
using ::testing::ElementsAreArray;

using donner::TestTempDir;

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

Vector2d TransformJsonPoint(const json& transformJson, const Vector2d& point) {
  const json& matrix = transformJson["matrix"];
  return Vector2d(matrix[0].get<double>() * point.x + matrix[2].get<double>() * point.y +
                      matrix[4].get<double>(),
                  matrix[1].get<double>() * point.x + matrix[3].get<double>() * point.y +
                      matrix[5].get<double>());
}

void ExpectNearTransformJson(std::string_view label, const json& actual, const json& expected) {
  ASSERT_TRUE(actual["matrix"].is_array()) << label << " actual=" << actual.dump(2);
  ASSERT_TRUE(expected["matrix"].is_array()) << label << " expected=" << expected.dump(2);
  ASSERT_EQ(actual["matrix"].size(), 6u) << label << " actual=" << actual.dump(2);
  ASSERT_EQ(expected["matrix"].size(), 6u) << label << " expected=" << expected.dump(2);

  for (std::size_t i = 0; i < 6; ++i) {
    EXPECT_NEAR(actual["matrix"][i].get<double>(), expected["matrix"][i].get<double>(), 1e-6)
        << label << " matrix index " << i << " actual=" << actual.dump(2)
        << " expected=" << expected.dump(2);
  }
}

json ExpectedEffectiveDragTranslation(const json& frame, const json& tile) {
  const json& active = frame["active_drag_preview"];
  const json& displayed = frame["displayed_drag_preview"];
  const json& cached = tile["drag_translation_doc"];
  if (!active.is_object() || !displayed.is_object() ||
      active.value("entity", 0u) != displayed.value("entity", 0u)) {
    return cached;
  }

  return json{
      {"x", cached.value("x", 0.0) + active["translation_doc"].value("x", 0.0) -
                displayed["translation_doc"].value("x", 0.0)},
      {"y", cached.value("y", 0.0) + active["translation_doc"].value("y", 0.0) -
                displayed["translation_doc"].value("y", 0.0)},
  };
}

MATCHER_P(HasPreviewTileSignature, expected, "") {
  const std::vector<std::string> actual = PreviewTileSignature(arg);
  if (actual != expected) {
    *result_listener << "actual signature=" << json(actual).dump();
    return false;
  }
  return true;
}

MATCHER_P(HasPreviewTileGenerationSignature, expected, "") {
  const std::vector<std::string> actual = PreviewTileGenerationSignature(arg);
  if (actual != expected) {
    *result_listener << "actual generation signature=" << json(actual).dump();
    return false;
  }
  return true;
}

MATCHER(IsPixelmatchIdentitySummary, "") {
  if (!arg.value("available", false)) {
    *result_listener << "diff unavailable: " << arg.dump();
    return false;
  }
  if (arg.value("comparison", "") != "pixelmatch") {
    *result_listener << "comparison=" << arg.value("comparison", "");
    return false;
  }
  if (arg.value("differing_pixels", -1) != 0) {
    *result_listener << "differing_pixels=" << arg.value("differing_pixels", -1)
                     << " artifacts=" << arg.value("artifacts", json::object()).dump();
    return false;
  }
  return true;
}

MATCHER(IsTilesPresentedFrameForActiveDrag, "") {
  if (!arg["active_drag_preview"].is_object()) {
    *result_listener << "active_drag_preview missing: " << arg.dump();
    return false;
  }
  const std::uint32_t activeEntity =
      arg["active_drag_preview"].value("entity", static_cast<std::uint32_t>(0));
  if (activeEntity == 0u) {
    *result_listener << "active drag entity is null";
    return false;
  }
  if (arg.value("path", "") == "tiles") {
    const std::uint32_t displayedEntity =
        arg.value("displayed_entity", static_cast<std::uint32_t>(0));
    if (displayedEntity != activeEntity) {
      *result_listener << "displayed_entity=" << displayedEntity
                       << " active_entity=" << activeEntity
                       << " tile generations=" << json(PreviewTileGenerationSignature(arg)).dump();
      return false;
    }
  }
  return true;
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

repro::ReproExpectation LoadFixtureExpectation(std::string_view rnrPath) {
  std::optional<repro::ReproFile> file =
      repro::ReadReproFile(std::filesystem::path(std::string(rnrPath)));
  EXPECT_TRUE(file.has_value()) << rnrPath;
  EXPECT_TRUE(file.has_value() && file->metadata.expect.has_value()) << rnrPath;
  if (!file.has_value() || !file->metadata.expect.has_value()) {
    return {};
  }
  return *file->metadata.expect;
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
    EXPECT_THAT(signatures[i], ElementsAreArray(expected)) << label << " stage " << i;
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
    EXPECT_THAT(firstDisplay, HasPreviewTileSignature(*expectedBeforeFirstRender)) << label;
  } else {
    const std::string path = firstDisplay.value("path", "");
    EXPECT_TRUE(path == "tiles" || path == "empty") << label << " path=" << path;
  }

  for (size_t i = 1; i < frames.size(); ++i) {
    const json& display = frames[i]["display_before_render"];
    EXPECT_EQ(display.value("path", ""), "tiles") << label << " frame " << i;
    EXPECT_THAT(display, HasPreviewTileSignature(expectedAfterFirstRender))
        << label << " frame " << i;
  }
}

void ExpectPresentedFrameMatchesFinalAfterClick(const json& frames,
                                                const repro::ReproExpectation& expect) {
  const json* targetFrame = nullptr;
  for (const json& frame : frames) {
    const auto offsetIt = frame.find("left_mouse_down_compare_offset");
    if (offsetIt == frame.end() || !offsetIt->is_number_integer() ||
        offsetIt->get<int>() != expect.frameOffsetAfterLeftMouseDown) {
      continue;
    }
    const int ordinal = frame.value("left_mouse_down_ordinal", 0);
    if (ordinal == 0 || ordinal == expect.leftMouseDownOrdinal) {
      targetFrame = &frame;
      break;
    }
  }

  ASSERT_NE(targetFrame, nullptr)
      << "GUI-scheduled replay did not store the expected presented frame. Stored frames: "
      << frames.dump(2);
  EXPECT_GE(targetFrame->value("frame_index", 0), expect.minFrameIndex);
  EXPECT_LE(targetFrame->value("frame_index", 0), expect.maxFrameIndex);

  EXPECT_THAT((*targetFrame)["presented_frame"], IsTilesPresentedFrameForActiveDrag());
  EXPECT_THAT((*targetFrame)["presented_frame_diff_from_eventual_final"],
              IsPixelmatchIdentitySummary())
      << "Frame " << targetFrame->value("frame_index", 0)
      << ", display path=" << (*targetFrame)["presented_frame"].value("path", "")
      << ", tile generations="
      << json(PreviewTileGenerationSignature((*targetFrame)["presented_frame"])).dump();
}

TEST(EditorControlSessionTest, ToolListExposesSelectorDragAndRenderTools) {
  const json tools = EditorControlSession::toolList();

  const json* replayTool = nullptr;
  auto hasTool = [&](std::string_view name) {
    for (const json& tool : tools) {
      if (tool.value("name", "") == name) {
        if (name == "replay_rnr") {
          replayTool = &tool;
        }
        return true;
      }
    }
    return false;
  };

  EXPECT_TRUE(hasTool("select_by_selector"));
  EXPECT_TRUE(hasTool("click_layer_button"));
  EXPECT_TRUE(hasTool("drag_selector"));
  EXPECT_TRUE(hasTool("transform_selector"));
  EXPECT_TRUE(hasTool("render_frame"));
  EXPECT_TRUE(hasTool("get_svg_source"));
  EXPECT_TRUE(hasTool("edit_svg_source"));
  EXPECT_TRUE(hasTool("set_active_tool"));
  EXPECT_TRUE(hasTool("set_style_property"));
  EXPECT_TRUE(hasTool("pen_path"));
  EXPECT_TRUE(hasTool("start_rnr_recording"));
  EXPECT_TRUE(hasTool("stop_rnr_recording"));
  EXPECT_TRUE(hasTool("replay_rnr"));
  ASSERT_NE(replayTool, nullptr);
  EXPECT_TRUE((*replayTool)["inputSchema"]["properties"].contains("gl_drive_document_input"));
}

TEST(EditorControlSessionTest, ClickLayerVisibilityButtonCapturesImmediateAndSettledFrames) {
  constexpr std::string_view kScene =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 40 20">
  <rect id="Background" width="40" height="20" fill="red"/>
  <rect id="Foreground" x="12" y="4" width="16" height="12" fill="blue"/>
</svg>)svg";

  EditorControlSession session;
  ToolCallResult load = session.handleToolCall("load_svg", json{{"svg_source", std::string(kScene)},
                                                                {"canvas_width", 40},
                                                                {"canvas_height", 20},
                                                                {"render_after_load", true}});
  ASSERT_TRUE(load.body.value("ok", false)) << load.body.dump(2);

  ToolCallResult click =
      session.handleToolCall("click_layer_button", json{{"selector", "#Background"},
                                                        {"button", "visibility"},
                                                        {"include_display_before_render", true},
                                                        {"include_final_frame", true},
                                                        {"include_display_frame", true}});

  ASSERT_TRUE(click.body.value("ok", false)) << click.body.dump(2);
  EXPECT_TRUE(click.body.value("mutation_queued", false));
  EXPECT_TRUE(click.body["before"].value("visible", false));
  ASSERT_TRUE(click.body["after"].is_object()) << click.body.dump(2);
  EXPECT_FALSE(click.body["after"].value("visible", true));
  EXPECT_EQ(click.body["display_before_render"].value("path", ""), "tiles");
  EXPECT_GT(click.body["display_before_render"].value("tile_count", 0), 0);
  EXPECT_TRUE(click.body["display_before_render_bitmap"].is_object()) << click.body.dump(2);
  ASSERT_TRUE(click.body["render_stages"].is_array()) << click.body.dump(2);
  ASSERT_FALSE(click.body["render_stages"].empty()) << click.body.dump(2);
  EXPECT_GT(click.body.value("attached_image_count", 0), 0);

  ToolCallResult source = session.handleToolCall("get_svg_source", json::object());
  ASSERT_TRUE(source.body.value("ok", false)) << source.body.dump(2);
  EXPECT_NE(source.body.value("text", "").find(R"(display="none")"), std::string::npos);
}

TEST(EditorControlSessionTest, HidingSplashBackgroundDropsGhostPixelsFromSettledFrame) {
  std::ifstream splash("donner_splash.svg", std::ios::binary);
  if (!splash.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ostringstream buffer;
  buffer << splash.rdbuf();

  EditorControlSession session;
  ToolCallResult load = session.handleToolCall("load_svg", json{{"svg_source", buffer.str()},
                                                                {"canvas_width", 892},
                                                                {"canvas_height", 512},
                                                                {"render_after_load", true}});
  ASSERT_TRUE(load.body.value("ok", false)) << load.body.dump(2);

  ToolCallResult click =
      session.handleToolCall("click_layer_button", json{{"selector", "#Background"},
                                                        {"button", "visibility"},
                                                        {"include_display_before_render", true},
                                                        {"include_final_frame", true},
                                                        {"include_display_frame", true}});

  ASSERT_TRUE(click.body.value("ok", false)) << click.body.dump(2);
  ASSERT_TRUE(click.body["after"].is_object()) << click.body.dump(2);
  EXPECT_FALSE(click.body["after"].value("visible", true));
  ASSERT_TRUE(click.body["display_before_render_bitmap"]["alpha"].is_object())
      << click.body.dump(2);
  EXPECT_EQ(click.body["display_before_render_bitmap"]["alpha"]["corner_samples"]["top_left"]["a"],
            255)
      << "The immediate display capture documents the confirmed stale pre-render frame.";

  ASSERT_TRUE(click.body["render_stages"].is_array()) << click.body.dump(2);
  ASSERT_FALSE(click.body["render_stages"].empty()) << click.body.dump(2);
  const json& finalBitmap = click.body["render_stages"].back()["bitmap"];
  ASSERT_TRUE(finalBitmap["alpha"].is_object()) << click.body.dump(2);
  EXPECT_EQ(finalBitmap["alpha"]["corner_samples"]["top_left"]["a"], 0)
      << "After #Background is hidden and the render settles, the splash corner should be "
         "transparent instead of retaining the old full-canvas background tile.";
  EXPECT_GT(finalBitmap["alpha"].value("transparent_pixels", 0u), 0u) << finalBitmap.dump(2);

  ASSERT_TRUE(click.body.contains("display_after_render")) << click.body.dump(2);
  ASSERT_TRUE(click.body.contains("display_after_render_bitmap")) << click.body.dump(2);
  const json& displayAfterBitmap = click.body.at("display_after_render_bitmap");
  ASSERT_TRUE(displayAfterBitmap.at("alpha").is_object()) << click.body.dump(2);
  EXPECT_EQ(displayAfterBitmap.at("alpha").at("corner_samples").at("top_left").at("a"), 0)
      << "The presented frame after hiding #Background must match the settled render. A nonzero "
         "corner alpha here is a stale texture/cache pixel, even when the renderer bitmap is "
         "already transparent.";
  EXPECT_GT(displayAfterBitmap.at("alpha").value("transparent_pixels", 0u), 0u)
      << displayAfterBitmap.dump(2);
}

TEST(EditorControlSessionTest, DraggingSplashBackgroundDropsGhostPixelsFromPresentedFrame) {
  std::ifstream splash("donner_splash.svg", std::ios::binary);
  if (!splash.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ostringstream buffer;
  buffer << splash.rdbuf();

  EditorControlSession session;
  ToolCallResult load = session.handleToolCall("load_svg", json{{"svg_source", buffer.str()},
                                                                {"canvas_width", 892},
                                                                {"canvas_height", 512},
                                                                {"render_after_load", true}});
  ASSERT_TRUE(load.body.value("ok", false)) << load.body.dump(2);

  ToolCallResult drag =
      session.handleToolCall("drag_selector", json{{"selector", "#Background"},
                                                   {"delta_x", 96.0},
                                                   {"delta_y", 0.0},
                                                   {"frames", 1},
                                                   {"release", false},
                                                   {"include_final_frame", false},
                                                   {"include_display_frame", true}});

  ASSERT_TRUE(drag.body.value("ok", false)) << drag.body.dump(2);
  ASSERT_TRUE(drag.body["frames"].is_array()) << drag.body.dump(2);
  ASSERT_GE(drag.body["frames"].size(), 2u) << drag.body.dump(2);
  const json& moveFrame = drag.body["frames"][1];
  ASSERT_EQ(moveFrame.value("label", ""), "move_1") << moveFrame.dump(2);
  ASSERT_TRUE(moveFrame["display_before_render_bitmap"]["alpha"].is_object()) << moveFrame.dump(2);
  EXPECT_EQ(moveFrame["display_before_render_bitmap"]["alpha"]["corner_samples"]["top_left"]["a"],
            0)
      << "The immediate held-drag display capture should expose transparent canvas at the old "
         "left edge before the next render result lands.";
  ASSERT_TRUE(moveFrame["display_after_render_bitmap"]["alpha"].is_object()) << moveFrame.dump(2);
  EXPECT_EQ(moveFrame["display_after_render_bitmap"]["alpha"]["corner_samples"]["top_left"]["a"], 0)
      << "After #Background moves right during an active drag, the presented frame should expose "
         "transparent canvas at the old left edge instead of keeping stale background pixels.";
  EXPECT_GT(moveFrame["display_after_render_bitmap"]["alpha"].value("transparent_pixels", 0u), 0u)
      << moveFrame["display_after_render_bitmap"].dump(2);
}

TEST(EditorControlSessionTest, InvalidToolCallReturnsErrorResult) {
  EditorControlSession session;

  ToolCallResult result = session.handleToolCall("drag_selector", json::object());

  EXPECT_TRUE(result.isError);
  EXPECT_TRUE(result.body["error"].is_string());
}

TEST(EditorControlSessionTest, EditsSvgSourceRangeAndRendersPreview) {
  constexpr std::string_view kScene =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 40 20">
  <rect id="target" width="40" height="20" fill="red"/>
</svg>)svg";

  EditorControlSession session;
  ToolCallResult load = session.handleToolCall("load_svg", json{{"svg_source", std::string(kScene)},
                                                                {"canvas_width", 40},
                                                                {"canvas_height", 20},
                                                                {"render_after_load", false}});
  ASSERT_TRUE(load.body.value("ok", false)) << load.body.dump(2);

  ToolCallResult source = session.handleToolCall("get_svg_source", json::object());
  ASSERT_TRUE(source.body.value("ok", false)) << source.body.dump(2);
  const std::string text = source.body.value("text", "");
  const std::size_t fillOffset = text.find("red");
  ASSERT_NE(fillOffset, std::string::npos);
  const std::uint64_t revision =
      source.body["source"].value("source_revision", static_cast<std::uint64_t>(0));

  ToolCallResult edit = session.handleToolCall(
      "edit_svg_source",
      json{{"expected_source_revision", revision},
           {"edits",
            json::array({json{{"offset", fillOffset}, {"delete_count", 3}, {"insert", "blue"}}})},
           {"canvas_width", 40},
           {"canvas_height", 20},
           {"include_final_frame", true}});
  ASSERT_TRUE(edit.body.value("ok", false)) << edit.body.dump(2);
  EXPECT_TRUE(edit.body.value("parsed", false));
  EXPECT_FALSE(edit.body.value("preview_stale", true));
  EXPECT_FALSE(edit.images.empty());

  ToolCallResult editedSource = session.handleToolCall("get_svg_source", json::object());
  ASSERT_TRUE(editedSource.body.value("ok", false)) << editedSource.body.dump(2);
  EXPECT_NE(editedSource.body.value("text", "").find("fill=\"blue\""), std::string::npos);
}

TEST(EditorControlSessionTest, InvalidSvgSourceEditKeepsDraftAndStalePreview) {
  constexpr std::string_view kScene =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 40 20">
  <rect id="target" width="40" height="20" fill="red"/>
</svg>)svg";
  constexpr std::string_view kFixedScene =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 40 20">
  <circle id="target" cx="20" cy="10" r="8" fill="white" stroke="black"/>
</svg>)svg";

  EditorControlSession session;
  ToolCallResult load = session.handleToolCall("load_svg", json{{"svg_source", std::string(kScene)},
                                                                {"canvas_width", 40},
                                                                {"canvas_height", 20},
                                                                {"render_after_load", false}});
  ASSERT_TRUE(load.body.value("ok", false)) << load.body.dump(2);
  const std::uint64_t initialRevision =
      load.body["source"].value("source_revision", static_cast<std::uint64_t>(0));

  ToolCallResult invalid = session.handleToolCall(
      "edit_svg_source", json{{"expected_source_revision", initialRevision},
                              {"replace_source", "<svg xmlns=\"http://www.w3.org/2000/svg\">"},
                              {"render_after_edit", true}});
  ASSERT_TRUE(invalid.body.value("ok", false)) << invalid.body.dump(2);
  EXPECT_FALSE(invalid.body.value("parsed", true));
  EXPECT_TRUE(invalid.body.value("preview_stale", false));
  EXPECT_TRUE(invalid.body["parse_error"].is_string());
  EXPECT_TRUE(invalid.images.empty());

  ToolCallResult staleGuard =
      session.handleToolCall("edit_svg_source", json{{"expected_source_revision", initialRevision},
                                                     {"replace_source", std::string(kFixedScene)}});
  EXPECT_TRUE(staleGuard.isError);

  ToolCallResult draft = session.handleToolCall("get_svg_source", json::object());
  ASSERT_TRUE(draft.body.value("ok", false)) << draft.body.dump(2);
  EXPECT_EQ(draft.body.value("text", ""), "<svg xmlns=\"http://www.w3.org/2000/svg\">");
  EXPECT_TRUE(draft.body["source"].value("preview_stale", false));

  const std::uint64_t draftRevision =
      draft.body["source"].value("source_revision", static_cast<std::uint64_t>(0));
  ToolCallResult fixed =
      session.handleToolCall("edit_svg_source", json{{"expected_source_revision", draftRevision},
                                                     {"replace_source", std::string(kFixedScene)},
                                                     {"render_after_edit", false}});
  ASSERT_TRUE(fixed.body.value("ok", false)) << fixed.body.dump(2);
  EXPECT_TRUE(fixed.body.value("parsed", false));
  EXPECT_FALSE(fixed.body["source"].value("preview_stale", true));
}

TEST(EditorControlSessionTest, PreDragRenderUsesFullCanvasCompositedTile) {
  EditorControlSession session;

  ToolCallResult load =
      session.handleToolCall("load_svg", json{{"svg_source", std::string(kFilteredScene)},
                                              {"canvas_width", 120},
                                              {"canvas_height", 80},
                                              {"render_after_load", true}});
  ASSERT_TRUE(load.body.value("ok", false)) << load.body.dump(2);
  ASSERT_TRUE(load.body.contains("render_stages"));
  ASSERT_FALSE(load.body["render_stages"].empty());

  const json& stage = load.body["render_stages"].back();
  ASSERT_FALSE(stage["composited_preview"].is_null());
  EXPECT_EQ(stage["composited_preview"].value("tile_count", 0), 1);
  ASSERT_EQ(stage["composited_preview"]["tiles"].size(), 1u);
  EXPECT_EQ(stage["composited_preview"]["tiles"][0].value("kind", ""), "segment");
  EXPECT_EQ(stage["composited_preview"]["tiles"][0].value("id", ""), "full-canvas");

  const json& display = stage["display_preview"];
  EXPECT_EQ(display.value("path", ""), "tiles");
  EXPECT_EQ(display.value("tile_count", 0), 1);
  ASSERT_EQ(display["tiles"].size(), 1u);
  EXPECT_EQ(display["tiles"][0].value("kind", ""), "segment");
  EXPECT_EQ(display["tiles"][0].value("id", ""), "full-canvas");
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

TEST(EditorControlSessionTest, TransformSelectorResizesThroughHandleAndExposesAffinePreview) {
  constexpr std::string_view kScene = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 80" width="100" height="80">
  <rect width="100" height="80" fill="white"/>
  <rect id="target" x="20" y="20" width="40" height="20" fill="blue"/>
</svg>
)svg";

  EditorControlSession session;
  ToolCallResult load = session.handleToolCall("load_svg", json{{"svg_source", std::string(kScene)},
                                                                {"canvas_width", 100},
                                                                {"canvas_height", 80},
                                                                {"render_after_load", true}});
  ASSERT_TRUE(load.body.value("ok", false)) << load.body.dump(2);

  ToolCallResult resize =
      session.handleToolCall("transform_selector", json{{"selector", "#target"},
                                                        {"mode", "scale"},
                                                        {"corner", "bottom_right"},
                                                        {"delta_x", 20.0},
                                                        {"delta_y", 10.0},
                                                        {"frames", 2},
                                                        {"release", false},
                                                        {"include_final_frame", false}});
  ASSERT_TRUE(resize.body.value("ok", false)) << resize.body.dump(2);
  EXPECT_EQ(resize.body.value("gesture_kind", ""), "resize");
  EXPECT_EQ(resize.body.value("corner", ""), "bottom_right");

  const json& lastFrame = resize.body["frames"].back();
  ASSERT_TRUE(lastFrame["active_gesture_preview"].is_object()) << lastFrame.dump(2);
  EXPECT_EQ(lastFrame["active_gesture_preview"].value("kind", ""), "resize");
  const json& activePreview = lastFrame["display_before_render"]["active_drag_preview"];
  ASSERT_TRUE(activePreview.is_object()) << lastFrame.dump(2);
  ASSERT_TRUE(activePreview["document_from_cached_document"].is_object()) << activePreview.dump(2);
  EXPECT_FALSE(activePreview["document_from_cached_document"].value("is_translation", true));

  const Vector2d anchoredCorner =
      TransformJsonPoint(activePreview["document_from_cached_document"], Vector2d(20.0, 20.0));
  const Vector2d activeCorner =
      TransformJsonPoint(activePreview["document_from_cached_document"], Vector2d(60.0, 40.0));
  EXPECT_NEAR(anchoredCorner.x, 20.0, 1e-6);
  EXPECT_NEAR(anchoredCorner.y, 20.0, 1e-6);
  EXPECT_NEAR(activeCorner.x, 80.0, 1e-6);
  EXPECT_NEAR(activeCorner.y, 50.0, 1e-6);

  bool sawAffinePresentationTile = false;
  for (const json& tile : lastFrame["display_before_render"]["tiles"]) {
    if (!tile.value("is_drag_target", false)) {
      continue;
    }
    ASSERT_TRUE(tile["effective_document_from_cached_document"].is_object()) << tile.dump(2);
    EXPECT_FALSE(tile["effective_document_from_cached_document"].value("is_translation", true));
    sawAffinePresentationTile = true;
  }
  EXPECT_TRUE(sawAffinePresentationTile) << lastFrame["display_before_render"].dump(2);
}

TEST(EditorControlSessionTest, LargeScaleDragKeepsPresentedTileTransformAlignedWithHandle) {
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

  ToolCallResult select = session.handleToolCall("select_by_selector",
                                                 json{{"selector", "#Donner_D"}, {"render", true}});
  ASSERT_TRUE(select.body.value("ok", false)) << select.body.dump(2);
  ASSERT_TRUE(select.body["selection_bounds_doc"].is_object()) << select.body.dump(2);
  const json& bounds = select.body["selection_bounds_doc"];
  const double deltaX = bounds["size"].value("x", 0.0) * 2.0;
  const double deltaY = bounds["size"].value("y", 0.0) * 2.0;

  ToolCallResult scale =
      session.handleToolCall("transform_selector", json{{"selector", "#Donner_D"},
                                                        {"mode", "scale"},
                                                        {"corner", "bottom_right"},
                                                        {"delta_x", deltaX},
                                                        {"delta_y", deltaY},
                                                        {"frames", 8},
                                                        {"release", false},
                                                        {"include_final_frame", false}});
  ASSERT_TRUE(scale.body.value("ok", false)) << scale.body.dump(2);

  bool sawRecapturedDragTargetTile = false;
  for (const json& frame : scale.body["frames"]) {
    SCOPED_TRACE(frame.value("label", ""));
    const json& display = frame["display_before_render"];
    ASSERT_TRUE(display["active_drag_preview"].is_object()) << display.dump(2);
    const json& activeTransform = display["active_drag_preview"]["document_from_cached_document"];

    for (const json& tile : display["tiles"]) {
      if (!tile.value("is_drag_target", false)) {
        continue;
      }

      if (!tile["document_from_cached_document"].value("is_identity", true)) {
        sawRecapturedDragTargetTile = true;
      }
      ExpectNearTransformJson("effective drag-target tile transform",
                              tile["effective_document_from_cached_document"], activeTransform);
    }
  }

  EXPECT_TRUE(sawRecapturedDragTargetTile) << scale.body.dump(2);
}

TEST(EditorControlSessionTest, TransformSelectorRotatesThroughHandleAndExposesAffinePreview) {
  constexpr std::string_view kScene = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 80 80" width="80" height="80">
  <rect width="80" height="80" fill="white"/>
  <rect id="target" x="20" y="20" width="20" height="20" fill="blue"/>
</svg>
)svg";

  EditorControlSession session;
  ToolCallResult load = session.handleToolCall("load_svg", json{{"svg_source", std::string(kScene)},
                                                                {"canvas_width", 80},
                                                                {"canvas_height", 80},
                                                                {"render_after_load", true}});
  ASSERT_TRUE(load.body.value("ok", false)) << load.body.dump(2);

  ToolCallResult rotate =
      session.handleToolCall("transform_selector", json{{"selector", "#target"},
                                                        {"mode", "rotate"},
                                                        {"corner", "top_right"},
                                                        {"delta_x", 0.0},
                                                        {"delta_y", 48.0},
                                                        {"frames", 2},
                                                        {"release", false},
                                                        {"include_final_frame", false}});
  ASSERT_TRUE(rotate.body.value("ok", false)) << rotate.body.dump(2);
  EXPECT_EQ(rotate.body.value("gesture_kind", ""), "rotate");
  EXPECT_EQ(rotate.body.value("corner", ""), "top_right");

  const json& lastFrame = rotate.body["frames"].back();
  ASSERT_TRUE(lastFrame["active_gesture_preview"].is_object()) << lastFrame.dump(2);
  EXPECT_EQ(lastFrame["active_gesture_preview"].value("kind", ""), "rotate");
  const json& activePreview = lastFrame["display_before_render"]["active_drag_preview"];
  ASSERT_TRUE(activePreview.is_object()) << lastFrame.dump(2);
  ASSERT_TRUE(activePreview["document_from_cached_document"].is_object()) << activePreview.dump(2);
  EXPECT_FALSE(activePreview["document_from_cached_document"].value("is_translation", true));

  const Vector2d center =
      TransformJsonPoint(activePreview["document_from_cached_document"], Vector2d(30.0, 30.0));
  const Vector2d topRight =
      TransformJsonPoint(activePreview["document_from_cached_document"], Vector2d(40.0, 20.0));
  EXPECT_NEAR(center.x, 30.0, 1e-6);
  EXPECT_NEAR(center.y, 30.0, 1e-6);
  EXPECT_NEAR(topRight.x, 40.0, 1e-6);
  EXPECT_NEAR(topRight.y, 40.0, 1e-6);
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
      "segment:640",
      "layer:9223372036854776448",
      "segment:2748779070096",
      "layer:9223372036854776464*",
      "segment:2817498546848",
      "layer:9223372036854776480",
      "layer:9223372036854776485",
      "segment:2907692860200",
      "layer:9223372036854776616",
      "layer:9223372036854776618",
      "layer:9223372036854776623",
      "segment:3500398347071",
      "layer:9223372036854776639",
      "segment:3569117822976",
  };
  const std::vector<std::string> expectedRDragOrder = {
      "segment:640",
      "layer:9223372036854776448",
      "segment:2748779070096",
      "layer:9223372036854776464",
      "segment:2817498546840",
      "layer:9223372036854776472*",
      "segment:2851858285216",
      "layer:9223372036854776480",
      "layer:9223372036854776485",
      "segment:2907692860200",
      "layer:9223372036854776616",
      "layer:9223372036854776618",
      "layer:9223372036854776623",
      "segment:3500398347071",
      "layer:9223372036854776639",
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

// TODO(#601): Re-enable once the multi-thread determinism test framework lands. This replay drives
// the async render worker, so the document is transiently `ThreadingMode::ConcurrentDom` while the
// UI thread performs unguarded live-document reads. Their timing relative to the worker's render
// window is nondeterministic, so the test either serializes the UI against the worker's write-held
// render phases (observed >70s here) or aborts on
// `ElementAnchor::assertScopedEntityHandleAccessAllowed` (`SVGElement.cc:253`). Tracked by the
// determinism-framework task (#601).
TEST(EditorControlSessionTest,
     DISABLED_ReplayFilteredElementFlashAfterDragsNeverReportsFlatDisplayPath) {
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

  for (const json& frame : frames) {
    if (!frame.contains("display_before_render")) {
      continue;
    }
    const json& display = frame["display_before_render"];
    const std::string displayPath = display.value("path", "");
    EXPECT_TRUE(displayPath == "tiles" || displayPath == "empty")
        << "Unexpected display path at frame " << frame.value("frame_index", 0) << ": "
        << displayPath << "\n"
        << frame.dump(2);
  }
}

// TODO(#601): Re-enable once the multi-thread determinism test framework lands. Same worker-race
// nondeterminism as DISABLED_ReplayFilteredElementFlashAfterDragsNeverReportsFlatDisplayPath: an
// unguarded UI-thread live-document read landing inside the worker's ConcurrentDom render window
// intermittently aborts on `ElementAnchor::assertScopedEntityHandleAccessAllowed`
// (`SVGElement.cc:253`). Tracked by the determinism-framework task (#601).
TEST(EditorControlSessionTest,
     DISABLED_ReplayFilteredElementFlashAfterDragsGuiScheduleDoesNotFlashOnSecondClick) {
  constexpr std::string_view kRnrPath =
      "donner/editor/tests/filtered-element-flash-after-drags-2.rnr";
  const repro::ReproExpectation expect = LoadFixtureExpectation(kRnrPath);

  EditorControlSession session;
  ToolCallResult replay = session.handleToolCall(
      "replay_rnr", json{{"rnr_path", std::string(kRnrPath)},
                         {"simulate_editor_shell_frame_loop", true},
                         {"include_frame_results", true},
                         {"include_display_diff", true},
                         {"compare_presented_after_left_mouse_down", expect.leftMouseDownOrdinal},
                         {"compare_presented_frame_offset_after_left_mouse_down",
                          expect.frameOffsetAfterLeftMouseDown},
                         {"max_frame_results", 240},
                         {"include_final_frame", false}});
  ASSERT_TRUE(replay.body.value("ok", false)) << replay.body.dump(2);

  const json& frames = replay.body["frames"];
  ASSERT_FALSE(frames.empty());

  ExpectPresentedFrameMatchesFinalAfterClick(frames, expect);
}

TEST(EditorControlSessionTest, RecordsAndReplaysRnrFromSelectorDrag) {
  const std::filesystem::path tempDir = TestTempDir();
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
  EXPECT_EQ(recorded->metadata.svgBasename, svgPath.filename().string());
  EXPECT_TRUE(recorded->metadata.svgContentHash.starts_with("fnv1a64:"));
  ASSERT_TRUE(recorded->metadata.svgSource.has_value());
  EXPECT_EQ(*recorded->metadata.svgSource, kFilteredScene);
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
  std::error_code removeError;
  std::filesystem::remove(svgPath, removeError);

  ToolCallResult replay =
      session.handleToolCall("replay_rnr", json{{"rnr_path", idleRnrPath.string()},
                                                {"include_frame_results", true},
                                                {"include_display_diff", true},
                                                {"max_frame_results", 10},
                                                {"include_final_frame", false}});
  ASSERT_TRUE(replay.body.value("ok", false));
  EXPECT_TRUE(replay.body.value("embedded_svg_source", false));
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
      for (const json& tile : beforeRender["tiles"]) {
        if (!tile.value("is_drag_target", false)) {
          continue;
        }
        EXPECT_EQ(tile["effective_drag_translation_doc"],
                  ExpectedEffectiveDragTranslation(beforeRender, tile))
            << "active drags should draw cached drag-target tiles at their cached offset plus "
               "the residual active-minus-displayed drag delta";
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

TEST(EditorControlSessionTest, RecordsInMemoryRnrWithEmbeddedSource) {
  const std::filesystem::path tempDir = TestTempDir();
  const std::filesystem::path rnrPath = tempDir / "donner_editor_control_rnr_memory.rnr";

  EditorControlSession session;
  ToolCallResult load =
      session.handleToolCall("load_svg", json{{"svg_source", std::string(kFilteredScene)},
                                              {"canvas_width", 120},
                                              {"canvas_height", 80},
                                              {"render_after_load", false}});
  ASSERT_TRUE(load.body.value("ok", false));

  ToolCallResult start =
      session.handleToolCall("start_rnr_recording", json{{"output_path", rnrPath.string()}});
  ASSERT_TRUE(start.body.value("ok", false)) << start.body.dump(2);
  EXPECT_EQ(start.body.value("svg_path", ""), "embedded.svg");
  EXPECT_TRUE(start.body.value("embedded_svg_source", false));

  ToolCallResult stop = session.handleToolCall("stop_rnr_recording", json::object());
  ASSERT_TRUE(stop.body.value("ok", false));

  std::optional<repro::ReproFile> recorded = repro::ReadReproFile(rnrPath);
  ASSERT_TRUE(recorded.has_value());
  EXPECT_EQ(recorded->metadata.svgPath, "embedded.svg");
  EXPECT_EQ(recorded->metadata.svgBasename, "embedded.svg");
  EXPECT_TRUE(recorded->metadata.svgContentHash.starts_with("fnv1a64:"));
  ASSERT_TRUE(recorded->metadata.svgSource.has_value());
  EXPECT_EQ(*recorded->metadata.svgSource, kFilteredScene);

  ToolCallResult replay = session.handleToolCall(
      "replay_rnr", json{{"rnr_path", rnrPath.string()}, {"include_frame_results", false}});
  ASSERT_TRUE(replay.body.value("ok", false)) << replay.body.dump(2);
  EXPECT_TRUE(replay.body.value("embedded_svg_source", false));

  std::error_code removeError;
  std::filesystem::remove(rnrPath, removeError);
}

TEST(EditorControlSessionTest, RecordsAndReplaysPenPathPaintActions) {
  constexpr std::string_view kScene =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 80 80" width="80" height="80">
</svg>)svg";
  const std::filesystem::path tempDir = TestScratchDir();
  const std::filesystem::path rnrPath = tempDir / "donner_editor_control_pen_paint.rnr";

  EditorControlSession session;
  ToolCallResult load = session.handleToolCall("load_svg", json{{"svg_source", std::string(kScene)},
                                                                {"canvas_width", 80},
                                                                {"canvas_height", 80},
                                                                {"render_after_load", false}});
  ASSERT_TRUE(load.body.value("ok", false)) << load.body.dump(2);

  ToolCallResult start =
      session.handleToolCall("start_rnr_recording", json{{"output_path", rnrPath.string()}});
  ASSERT_TRUE(start.body.value("ok", false)) << start.body.dump(2);

  ToolCallResult tool = session.handleToolCall("set_active_tool", json{{"tool", "pen"}});
  ASSERT_TRUE(tool.body.value("ok", false)) << tool.body.dump(2);

  ToolCallResult fill = session.handleToolCall(
      "set_style_property",
      json{{"property", "fill"}, {"value", "#ff0000"}, {"render_after_set", false}});
  ASSERT_TRUE(fill.body.value("ok", false)) << fill.body.dump(2);
  EXPECT_FALSE(fill.body.value("queued_selection_mutation", true));

  ToolCallResult pen =
      session.handleToolCall("pen_path", json{{"points", json::array({
                                                             json{{"x", 10.0}, {"y", 10.0}},
                                                             json{{"x", 70.0}, {"y", 10.0}},
                                                             json{{"x", 40.0}, {"y", 70.0}},
                                                         })},
                                              {"close", true},
                                              {"include_final_frame", true}});
  ASSERT_TRUE(pen.body.value("ok", false)) << pen.body.dump(2);
  ASSERT_TRUE(pen.body["selection"].is_object()) << pen.body.dump(2);

  ToolCallResult stop = session.handleToolCall("stop_rnr_recording", json::object());
  ASSERT_TRUE(stop.body.value("ok", false)) << stop.body.dump(2);
  EXPECT_GT(stop.body.value("frame_count", 0), 0);

  std::optional<repro::ReproFile> recorded = repro::ReadReproFile(rnrPath);
  ASSERT_TRUE(recorded.has_value());
  bool sawPenToolAction = false;
  bool sawFillAction = false;
  for (const repro::ReproFrame& frame : recorded->frames) {
    for (const repro::ReproAction& action : frame.actions) {
      if (action.kind == repro::ReproAction::Kind::SetActiveTool && action.tool == "pen") {
        sawPenToolAction = true;
      }
      if (action.kind == repro::ReproAction::Kind::SetStyleProperty &&
          action.propertyName == "fill" && action.propertyValue == "#ff0000") {
        sawFillAction = true;
      }
    }
  }
  EXPECT_TRUE(sawPenToolAction);
  EXPECT_TRUE(sawFillAction);

  EditorControlSession replaySession;
  ToolCallResult replay =
      replaySession.handleToolCall("replay_rnr", json{{"rnr_path", rnrPath.string()},
                                                      {"render_each_frame", false},
                                                      {"include_frame_results", false}});
  ASSERT_TRUE(replay.body.value("ok", false)) << replay.body.dump(2);

  ToolCallResult source = replaySession.handleToolCall("get_svg_source", json::object());
  ASSERT_TRUE(source.body.value("ok", false)) << source.body.dump(2);
  const std::string text = source.body.value("text", "");
  EXPECT_NE(text.find("<path"), std::string::npos) << text;
  EXPECT_NE(text.find("fill: #ff0000"), std::string::npos) << text;
  EXPECT_NE(text.find(" Z"), std::string::npos) << text;

  std::error_code removeError;
  std::filesystem::remove(rnrPath, removeError);
}

TEST(EditorControlSessionTest, ChangingFillOnNewPenPathUpdatesSettledFrame) {
  constexpr std::string_view kScene =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 80 80" width="80" height="80">
</svg>)svg";

  EditorControlSession session;
  ASSERT_TRUE(session
                  .handleToolCall("load_svg", json{{"svg_source", std::string(kScene)},
                                                   {"canvas_width", 80},
                                                   {"canvas_height", 80},
                                                   {"render_after_load", false}})
                  .body.value("ok", false));

  ASSERT_TRUE(
      session.handleToolCall("set_active_tool", json{{"tool", "pen"}}).body.value("ok", false));
  ASSERT_TRUE(session
                  .handleToolCall(
                      "set_style_property",
                      json{{"property", "fill"}, {"value", "none"}, {"render_after_set", false}})
                  .body.value("ok", false));
  ASSERT_TRUE(session
                  .handleToolCall(
                      "set_style_property",
                      json{{"property", "stroke"}, {"value", "none"}, {"render_after_set", false}})
                  .body.value("ok", false));

  ToolCallResult pen =
      session.handleToolCall("pen_path", json{{"points", json::array({
                                                             json{{"x", 10.0}, {"y", 10.0}},
                                                             json{{"x", 70.0}, {"y", 10.0}},
                                                             json{{"x", 40.0}, {"y", 70.0}},
                                                         })},
                                              {"close", true},
                                              {"include_final_frame", true}});
  ASSERT_TRUE(pen.body.value("ok", false)) << pen.body.dump(2);
  ASSERT_TRUE(pen.body["render_stages"].is_array()) << pen.body.dump(2);
  const json& beforeBitmap = pen.body["render_stages"].back()["bitmap"];
  ASSERT_TRUE(beforeBitmap["alpha"].is_object()) << beforeBitmap.dump(2);
  EXPECT_EQ(beforeBitmap["alpha"]["center_sample"]["a"], 0) << beforeBitmap.dump(2);

  ToolCallResult fill = session.handleToolCall(
      "set_style_property",
      json{{"property", "fill"}, {"value", "#ff0000"}, {"include_final_frame", true}});
  ASSERT_TRUE(fill.body.value("ok", false)) << fill.body.dump(2);
  ASSERT_TRUE(fill.body["render_stages"].is_array()) << fill.body.dump(2);
  const json& afterBitmap = fill.body["render_stages"].back()["bitmap"];
  ASSERT_TRUE(afterBitmap["alpha"].is_object()) << afterBitmap.dump(2);
  const json& center = afterBitmap["alpha"]["center_sample"];
  EXPECT_GT(center.value("r", 0), 220) << afterBitmap.dump(2);
  EXPECT_LT(center.value("g", 255), 40) << afterBitmap.dump(2);
  EXPECT_LT(center.value("b", 255), 40) << afterBitmap.dump(2);
  EXPECT_GT(center.value("a", 0), 220) << afterBitmap.dump(2);

  ToolCallResult source = session.handleToolCall("get_svg_source", json::object());
  ASSERT_TRUE(source.body.value("ok", false)) << source.body.dump(2);
  EXPECT_NE(source.body.value("text", "").find("fill: #ff0000"), std::string::npos);
}

}  // namespace
}  // namespace donner::editor::mcp
