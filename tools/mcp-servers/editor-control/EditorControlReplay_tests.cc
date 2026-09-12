/// @file
/// Native editor-control replay tests using the editor's product configuration.

#include <cstdint>
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
  const int selectAllModifiers = ImGuiIO{}.ConfigMacOSXBehaviors ? (1 << 3) : (1 << 0);
  for (std::uint64_t index = 0; index <= 44; ++index) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.mouseX = 200;
    frame.mouseY = 240;
    if (index >= 21) {
      frame.mouseX = 868;
      frame.mouseY = 375;
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
