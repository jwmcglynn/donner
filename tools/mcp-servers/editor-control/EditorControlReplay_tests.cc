/// @file
/// Native editor-control replay tests using the editor's product configuration.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

#include "donner/base/tests/TestTempDir.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/repro/GlRnrReplay.h"
#include "donner/editor/repro/ReproFile.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "tools/mcp-servers/editor-control/EditorControlSession.h"

namespace donner::editor::mcp {
namespace {

using nlohmann::json;

TEST(EditorControlSessionTest, AnimationPreviewToolbarCaptureAcrossWorkerHandoff) {
  repro::ReproFile replay;
  replay.metadata.svgPath = "animation_toolbar.svg";
  replay.metadata.svgSource =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="640" height="400">
<circle cx="40" cy="80" r="20" fill="#72dec2">
<animate attributeName="cx" from="40" to="600" dur="2s" repeatCount="indefinite"/>
</circle></svg>)svg";
  replay.metadata.windowWidth = 1600;
  replay.metadata.windowHeight = 900;
  replay.metadata.displayScale = 1.0;
  for (std::uint64_t index = 0; index <= 30; ++index) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.mouseX = index <= 4 ? 1435.0 : 1100.0;
    frame.mouseY = index <= 4 ? 20.0 : 800.0;
    if (index == 3) {
      frame.mouseButtonMask = 1;
      frame.events.push_back({.kind = repro::ReproEvent::Kind::MouseDown});
    } else if (index == 4) {
      frame.events.push_back({.kind = repro::ReproEvent::Kind::MouseUp});
    }
    replay.frames.push_back(std::move(frame));
  }
  const std::filesystem::path rnrPath = TestTempDir() / "animation_toolbar.rnr";
  ASSERT_THAT(repro::WriteReproFile(rnrPath, replay), testing::IsTrue());
  const char* outputs = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  const std::filesystem::path outputDir = outputs != nullptr ? outputs : TestTempDir();
  if (rnrPath != outputDir / "animation_toolbar.rnr") {
    std::filesystem::copy_file(rnrPath, outputDir / "animation_toolbar.rnr",
                               std::filesystem::copy_options::overwrite_existing);
  }
  for (const int frame : {12, 22}) {
    SCOPED_TRACE(frame);
    EditorControlSession session;
    const ToolCallResult result = session.handleToolCall(
        "replay_rnr", json{{"rnr_path", rnrPath.string()},
                           {"gl_readback", true},
                           {"gl_capture_frame", frame},
                           {"gl_max_frame", 30},
                           {"gl_crop", "full"},
                           {"gl_output_dir", (outputDir / std::to_string(frame)).string()},
                           {"gl_worker_document_hold_start_frame", 15},
                           {"gl_worker_document_hold_end_frame", 25},
                           {"gl_pace", true},
                           {"include_gl_images", false}});
    ASSERT_THAT(result.isError, testing::IsFalse()) << result.body.dump(2);
    EXPECT_THAT(result.body.value("capture_count", 0), testing::Eq(1));
  }
}

TEST(EditorControlSessionTest, HighDpiEditorStartupCapture) {
  repro::ReproFile replay;
  replay.metadata.svgPath = "font_scale.svg";
  replay.metadata.svgSource =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="640" height="400"/>)svg";
  replay.metadata.windowWidth = 960;
  replay.metadata.windowHeight = 640;
  replay.metadata.displayScale = 2.0;
  for (std::uint64_t index = 0; index <= 2; ++index) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    replay.frames.push_back(std::move(frame));
  }
  const std::filesystem::path rnrPath = TestTempDir() / "font_scale.rnr";
  ASSERT_THAT(repro::WriteReproFile(rnrPath, replay), testing::IsTrue());
  const char* outputs = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  const std::filesystem::path outputDir = outputs != nullptr ? outputs : TestTempDir();
  EditorControlSession session;
  const ToolCallResult result =
      session.handleToolCall("replay_rnr", json{{"rnr_path", rnrPath.string()},
                                                {"gl_readback", true},
                                                {"gl_capture_frame", 2},
                                                {"gl_crop", "full"},
                                                {"gl_output_dir", outputDir.string()},
                                                {"gl_pace", true},
                                                {"include_gl_images", false}});
  ASSERT_THAT(result.isError, testing::IsFalse()) << result.body.dump(2);
  EXPECT_THAT(result.body.value("capture_count", 0), testing::Eq(1)) << result.body.dump(2);
}

TEST(EditorControlSessionTest, HiDpiInspectorCapturePreservesResolvedLayout) {
  repro::ReproFile replay;
  replay.metadata.svgPath = "hidpi_inspector.svg";
  replay.metadata.svgSource =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80" viewBox="0 0 120 80">
<path id="target" d="M 10 12 L 50 12 L 50 36 Z" style="fill: #010203; stroke: #040506"/>
</svg>)svg";
  replay.metadata.windowWidth = 640;
  replay.metadata.windowHeight = 400;
  replay.metadata.displayScale = 2.0;
  for (std::uint64_t index = 0; index <= 36; ++index) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.mouseDocX = 35.0;
    frame.mouseDocY = 18.0;
    if (index == 3) {
      frame.mouseButtonMask = 1;
      frame.events.push_back({.kind = repro::ReproEvent::Kind::MouseDown});
    } else if (index == 4) {
      frame.events.push_back({.kind = repro::ReproEvent::Kind::MouseUp});
    }
    replay.frames.push_back(std::move(frame));
  }
  const char* outputs = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  const std::filesystem::path outputDir = outputs != nullptr ? outputs : TestTempDir();
  const std::filesystem::path rnrPath = outputDir / "hidpi_inspector.rnr";
  ASSERT_THAT(repro::WriteReproFile(rnrPath, replay), testing::IsTrue());
  EditorControlSession session;
  const ToolCallResult capture =
      session.handleToolCall("replay_rnr", json{{"rnr_path", rnrPath.string()},
                                                {"gl_readback", true},
                                                {"gl_capture_frame", 36},
                                                {"gl_crop", "full"},
                                                {"gl_output_dir", (outputDir / "mcp").string()},
                                                {"gl_drive_document_input", true},
                                                {"gl_pace", true},
                                                {"include_gl_images", false}});
  ASSERT_THAT(capture.isError, testing::IsFalse()) << capture.body.dump(2);
  ASSERT_THAT(capture.body.value("capture_count", 0), testing::Eq(1));

  // The MCP capture and diagnostic replay use the same real-clock editor journey. The last
  // frame is well after the 120 ms canvas debounce, without enabling document pixel capture.
  repro::GlRnrReplayOptions options;
  options.rnrPath = rnrPath;
  options.outputDir = outputDir / "diagnostics";
  options.captureFrames = {36};
  options.maxFrame = 36;
  options.driveDocumentSpaceInput = true;
  options.pace = true;
  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_THAT(repro::RunGlRnrReplay(options, &result, &error), testing::IsTrue()) << error;
  ASSERT_THAT(result.finalSelectedElementLabel, testing::Optional(testing::HasSubstr("target")));
  ASSERT_FALSE(result.frameDiagnostics.empty());
  std::uint64_t commits = 0;
  for (const auto& frame : result.frameDiagnostics) {
    commits += frame.frameCost.documentCanvasCommitCount;
  }
  EXPECT_THAT(commits, testing::Eq(0u));
  // Frame 35 has no capture selector: its rendering instance must already be fresh before
  // frame 36 reads the framebuffer. Capture barriers cannot supply the missing render.
  ASSERT_THAT(result.frameDiagnostics.size(), testing::Eq(37u));
  EXPECT_THAT(result.frameDiagnostics[35].selectedRenderingInstanceFill,
              testing::Optional(testing::Eq("PaintServer(solid rgba(1, 2, 3, 255))")));
  const auto& status = result.frameDiagnostics.back();
  EXPECT_THAT(status.selectedRenderingInstanceFill,
              testing::Optional(testing::Eq("PaintServer(solid rgba(1, 2, 3, 255))")));
  EXPECT_THAT(status.selectedStyleAttribute,
              testing::Optional(testing::Eq("fill: #010203; stroke: #040506")));
  EXPECT_THAT(status.selectedLocalStyleFill,
              testing::Optional(testing::Eq("PaintServer(solid rgba(1, 2, 3, 255))")));
  EXPECT_THAT(status.selectedComputedFill,
              testing::Optional(testing::Eq("PaintServer(solid rgba(1, 2, 3, 255))")));
  EXPECT_THAT(status.selectedPathDataAttribute,
              testing::Optional(testing::Eq("M 10 12 L 50 12 L 50 36 Z")));
}

TEST(EditorControlSessionTest, DragPathOverlayCapture) {
  repro::ReproFile replay;
  replay.metadata.svgPath = "drag_path.svg";
  replay.metadata.svgSource =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="640" height="400">
<path id="target" d="M100 260 L200 80 L300 260 Z" fill="#f97316"/>
</svg>)svg";
  replay.metadata.windowWidth = 960;
  replay.metadata.windowHeight = 640;
  replay.metadata.displayScale = 1.0;
  for (std::uint64_t index = 0; index <= 36; ++index) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.mouseDocX = index < 16 ? 200.0 : 250.0;
    frame.mouseDocY = index < 16 ? 180.0 : 210.0;
    if (index >= 10) {
      frame.mouseButtonMask = 1;
    }
    if (index == 10) {
      frame.events.push_back({.kind = repro::ReproEvent::Kind::MouseDown});
    }
    replay.frames.push_back(std::move(frame));
  }
  const std::filesystem::path rnrPath = TestTempDir() / "drag_path.rnr";
  ASSERT_THAT(repro::WriteReproFile(rnrPath, replay), testing::IsTrue());
  const char* outputs = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  const std::filesystem::path outputDir = outputs != nullptr ? outputs : TestTempDir();
  EditorControlSession session;
  for (const int frame : {14, 36}) {
    const ToolCallResult result =
        session.handleToolCall("replay_rnr", json{{"rnr_path", rnrPath.string()},
                                                  {"gl_readback", true},
                                                  {"gl_capture_frame", frame},
                                                  {"gl_crop", "document-canvas"},
                                                  {"gl_output_dir", outputDir.string()},
                                                  {"gl_drive_document_input", true},
                                                  {"gl_pace", true},
                                                  {"include_gl_images", false}});
    ASSERT_THAT(result.isError, testing::IsFalse()) << result.body.dump(2);
    EXPECT_THAT(result.body.value("capture_count", 0), testing::Eq(1)) << result.body.dump(2);
  }
}

TEST(EditorControlSessionTest, InspectorTextInputReplay) {
  repro::ReproFile replay;
  replay.metadata.svgPath = "inspector.svg";
  replay.metadata.svgSource =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="640" height="400">
<rect id="target" x="32" y="32" width="180" height="120" fill="#2563eb"/>
<rect id="other" x="300" y="32" width="80" height="80" fill="#f97316"/>
</svg>)svg";
  replay.metadata.windowWidth = 1100;
  replay.metadata.windowHeight = 720;
  replay.metadata.displayScale = 1.0;
  // The pointer coordinates below were authored against the document opening at 100%.
  replay.metadata.formatVersion = repro::kFirstReproVersionWithFittedOpen - 1;
  const bool nativeMacMenu = ImGuiIO{}.ConfigMacOSXBehaviors;
  const int selectAllModifiers = nativeMacMenu ? (1 << 3) : (1 << 0);
  // The non-macOS ImGui menu occupies space above the Inspector's Position X input.
  const double positionXInputY = nativeMacMenu ? 361.0 : 378.0;
  for (std::uint64_t index = 0; index <= 44; ++index) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.mouseX = 200;
    frame.mouseY = 240;
    if (index >= 21) {
      frame.mouseX = 868;
      frame.mouseY = positionXInputY;
    }
    if (index == 10 || index == 21 || index == 23) {
      frame.mouseButtonMask = 1;
      frame.events.push_back({.kind = repro::ReproEvent::Kind::MouseDown});
    } else if (index == 11 || index == 22 || index == 24) {
      frame.events.push_back({.kind = repro::ReproEvent::Kind::MouseUp});
    } else if (index == 28) {
      frame.modifiers = selectAllModifiers;
      frame.events.push_back({.kind = repro::ReproEvent::Kind::KeyDown, .key = ImGuiKey_A});
    } else if (index == 29) {
      frame.events.push_back({.kind = repro::ReproEvent::Kind::KeyUp, .key = ImGuiKey_A});
    } else if (index == 30 || index == 31) {
      frame.events.push_back({.kind = repro::ReproEvent::Kind::Char,
                              .codepoint = static_cast<std::uint32_t>(index == 30 ? '6' : '4')});
    } else if (index == 32) {
      frame.events.push_back({.kind = repro::ReproEvent::Kind::KeyDown, .key = ImGuiKey_Enter});
    } else if (index == 33) {
      frame.events.push_back({.kind = repro::ReproEvent::Kind::KeyUp, .key = ImGuiKey_Enter});
    }
    replay.frames.push_back(std::move(frame));
  }
  const std::filesystem::path rnrPath = TestTempDir() / "inspector_input.rnr";
  ASSERT_EQ(repro::WriteReproFile(rnrPath, replay), true);
  repro::GlRnrReplayOptions focusOptions;
  focusOptions.rnrPath = rnrPath;
  focusOptions.maxFrame = 29;
  focusOptions.captureFrames = {29};
  const char* outputs = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  focusOptions.outputDir = outputs != nullptr ? outputs : TestTempDir();
  repro::GlRnrReplayResult focusResult;
  std::string focusError;
  ASSERT_EQ(repro::RunGlRnrReplay(focusOptions, &focusResult, &focusError), true) << focusError;
  ASSERT_THAT(focusResult.finalSelectedElementLabel,
              ::testing::Optional(::testing::HasSubstr("target")))
      << "Select All must remain inside the focused Position X input";
  EditorControlSession session;
  const ToolCallResult result =
      session.handleToolCall("replay_rnr", json{{"rnr_path", rnrPath.string()},
                                                {"gl_readback", true},
                                                {"gl_capture_frame", 44},
                                                {"gl_pace", true},
                                                {"gl_timeout_ms", 5000},
                                                {"include_gl_images", false}});
  ASSERT_EQ(result.isError, false) << result.body.dump(2);
  EXPECT_EQ(result.body.value("capture_count", 0), 1) << result.body.dump(2);

  repro::GlRnrReplayOptions options;
  options.rnrPath = rnrPath;
  options.captureFrames = {44};
  options.outputDir = TestTempDir();
  repro::GlRnrReplayResult replayResult;
  std::string error;
  ASSERT_EQ(repro::RunGlRnrReplay(options, &replayResult, &error), true) << error;
  ASSERT_THAT(replayResult.finalDocumentSource, ::testing::Optional(::testing::_));
  const ToolCallResult load = session.handleToolCall(
      "load_svg",
      json{{"svg_source", *replayResult.finalDocumentSource}, {"render_after_load", false}});
  ASSERT_EQ(load.isError, false) << load.body.dump(2);
  const ToolCallResult selection = session.handleToolCall(
      "select_by_selector", json{{"selector", "#target"}, {"render", false}});
  ASSERT_EQ(selection.isError, false) << selection.body.dump(2);
  EXPECT_THAT(selection.body.at("selection_bounds_doc").at("top_left"),
              ::testing::Eq(json{{"x", 64.0}, {"y", 32.0}}));
}

}  // namespace
}  // namespace donner::editor::mcp
