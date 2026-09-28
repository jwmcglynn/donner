/// @file
/// Native splash drag reproduction through editor-control.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#include "donner/base/tests/TestTempDir.h"
#include "donner/editor/repro/GlRnrReplay.h"
#include "donner/editor/repro/ReproFile.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "tools/mcp-servers/editor-control/EditorControlSession.h"

namespace donner::editor::mcp {
namespace {

std::filesystem::path WriteSplashDragReplay(const std::filesystem::path& outputDir) {
  const char* srcdir = std::getenv("TEST_SRCDIR");
  const char* workspace = std::getenv("TEST_WORKSPACE");
  if (srcdir == nullptr || workspace == nullptr) {
    return {};
  }
  std::ifstream svg(std::filesystem::path(srcdir) / workspace / "donner_splash.svg");
  if (!svg.is_open()) {
    return {};
  }
  repro::ReproFile replay;
  replay.metadata.svgPath = "donner_splash.svg";
  replay.metadata.svgSource = std::string(std::istreambuf_iterator<char>(svg), {});
  replay.metadata.windowWidth = 1100;
  replay.metadata.windowHeight = 720;
  replay.metadata.displayScale = 2.0;
  for (std::uint64_t index = 0; index <= 30; ++index) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.viewport = repro::ReproViewport{.paneOriginX = 32.0,
                                          .paneOriginY = 0.0,
                                          .paneSizeW = 510.0,
                                          .paneSizeH = 360.0,
                                          .devicePixelRatio = 2.0,
                                          .zoom = 1.0,
                                          .panDocX = 302.0,
                                          .panDocY = 390.0,
                                          .panScreenX = 300.0,
                                          .panScreenY = 240.0,
                                          .viewBoxX = 0.0,
                                          .viewBoxY = 0.0,
                                          .viewBoxW = 892.0,
                                          .viewBoxH = 512.0};
    const double progress = std::clamp((static_cast<double>(index) - 16.0) / 6.0, 0.0, 1.0);
    frame.mouseDocX = 282.0 - progress * 80.0;
    frame.mouseDocY = 390.0 - progress * 65.0;
    frame.mouseX = 300.0 + *frame.mouseDocX - 302.0;
    frame.mouseY = 240.0 + *frame.mouseDocY - 390.0;
    if (index >= 16) {
      frame.mouseButtonMask = 1;
    }
    if (index == 16) {
      frame.events.push_back({.kind = repro::ReproEvent::Kind::MouseDown,
                              .mouseButton = 0,
                              .hit = repro::ReproHit{.id = "Donner_D", .tag = "path"}});
    }
    replay.frames.push_back(std::move(frame));
  }
  const std::filesystem::path rnrPath = outputDir / "splash_first_drag.rnr";
  return repro::WriteReproFile(rnrPath, replay) ? rnrPath : std::filesystem::path();
}

TEST(EditorControlSessionTest, SplashFirstDragCapture) {
  const char* outputs = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  const std::filesystem::path outputDir = outputs != nullptr ? outputs : TestTempDir();
  const std::filesystem::path rnrPath = WriteSplashDragReplay(outputDir);
  ASSERT_THAT(rnrPath.empty(), testing::IsFalse());
  EditorControlSession session;
  const ToolCallResult result = session.handleToolCall(
      "replay_rnr", nlohmann::json{{"rnr_path", rnrPath.string()},
                                   {"gl_readback", true},
                                   {"gl_capture_frame", 30},
                                   {"gl_crop", "document-canvas"},
                                   {"gl_output_dir", outputDir.string()},
                                   {"gl_drive_document_input", false},
                                   {"gl_pace", true},
                                   {"gl_worker_document_hold_start_frame", 16},
                                   {"gl_worker_document_hold_end_frame", 23},
                                   {"include_gl_images", false}});
  std::ofstream(outputDir / "mcp_result.json") << result.body.dump(2);
  ASSERT_THAT(result.isError, testing::IsFalse()) << result.body.dump(2);
  EXPECT_THAT(result.body.value("capture_count", 0), testing::Eq(1)) << result.body.dump(2);
}

TEST(EditorControlSessionTest, SplashFirstDragFrameTransforms) {
  const char* outputs = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  const std::filesystem::path outputDir = outputs != nullptr ? outputs : TestTempDir();
  const std::filesystem::path rnrPath = WriteSplashDragReplay(outputDir);
  ASSERT_THAT(rnrPath.empty(), testing::IsFalse());
  repro::GlRnrReplayOptions options;
  options.rnrPath = rnrPath;
  options.outputDir = outputDir;
  options.captureFrames = {16, 21, 23, 26, 30};
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.driveDocumentSpaceInput = false;
  options.pace = true;
  options.workerDocumentAccessHoldStartFrame = 16;
  options.workerDocumentAccessHoldEndFrame = 23;
  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_THAT(repro::RunGlRnrReplay(options, &result, &error), testing::IsTrue()) << error;
  ASSERT_THAT(result.finalSelectedElementLabel, testing::Optional(testing::HasSubstr("Donner_D")));
  nlohmann::json frames = nlohmann::json::array();
  int checkedFrames = 0;
  for (const auto& frame : result.frameDiagnostics) {
    nlohmann::json item{{"frame", frame.frameIndex},
                        {"document_version", frame.documentFrameVersion},
                        {"displayed_version", frame.displayedDocVersion},
                        {"tiles", nlohmann::json::array()}};
    const auto addPreview = [&](const char* name, const auto& preview) {
      if (preview.has_value()) {
        item[name] = {{"entity", static_cast<std::uint32_t>(preview->entity)},
                      {"generation", preview->dragGeneration},
                      {"translation", {preview->translation.x, preview->translation.y}}};
      }
    };
    addPreview("active", frame.activeDragPreview);
    addPreview("displayed", frame.displayedDragPreview);
    const auto& overlay = frame.frameCost.overlay;
    item["overlay_translation"] = {overlay.representedDragTranslationDoc.x,
                                   overlay.representedDragTranslationDoc.y};
    for (const auto& tile : frame.tiles) {
      item["tiles"].push_back(
          {{"id", tile.id},
           {"drag_target", tile.isDragTarget},
           {"offset", {tile.canvasOffsetDoc.x, tile.canvasOffsetDoc.y}},
           {"drag", {tile.dragTranslationDoc.x, tile.dragTranslationDoc.y}},
           {"presented",
            {tile.presentedDragTranslationDoc.x, tile.presentedDragTranslationDoc.y}}});
      if (tile.isDragTarget && frame.activeDragPreview.has_value() &&
          overlay.hasRepresentedDragPreview && frame.frameIndex > 20) {
        ++checkedFrames;
        EXPECT_THAT(tile.canvasOffsetDoc + tile.presentedDragTranslationDoc,
                    testing::Eq(Vector2d(270.0, 345.0) + overlay.representedDragTranslationDoc))
            << "frame " << frame.frameIndex;
      }
    }
    frames.push_back(std::move(item));
  }
  std::ofstream(outputDir / "splash_frame_diagnostics.json") << frames.dump(2);
  EXPECT_GT(checkedFrames, 0);
}

}  // namespace
}  // namespace donner::editor::mcp
