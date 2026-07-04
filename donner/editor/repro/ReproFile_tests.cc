#include "donner/editor/repro/ReproFile.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/repro/ReproRecorder.h"

namespace donner::editor::repro {

namespace {

std::filesystem::path TempFile(std::string_view stem) {
  const auto tmpDir = std::filesystem::temp_directory_path();
  return tmpDir / (std::string(stem) + "_" + std::to_string(std::rand()) + ".donner-repro");
}

ReproFile MakeFileWithOneFrame() {
  ReproFile file;
  file.metadata.svgPath = "foo/bar.svg";
  file.metadata.windowWidth = 1600;
  file.metadata.windowHeight = 900;
  file.metadata.displayScale = 2.0;
  file.metadata.experimentalMode = true;
  file.metadata.startedAtIso8601 = "2026-04-19T12:00:00Z";

  ReproFrame f0;
  f0.index = 0;
  f0.timestampSeconds = 0.0;
  f0.deltaMs = 16.667;
  f0.mouseX = 123.25;
  f0.mouseY = 456.75;
  f0.mouseButtonMask = 0;
  f0.modifiers = 0;
  file.frames.push_back(f0);

  return file;
}

class ImGuiContextGuard {
public:
  ImGuiContextGuard() {
    IMGUI_CHECKVERSION();
    context_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Build();
  }

  ~ImGuiContextGuard() {
    if (context_ != nullptr) {
      ImGui::DestroyContext(context_);
    }
  }

private:
  ImGuiContext* context_ = nullptr;
};

void BeginImGuiFrame(ImVec2 displaySize, float deltaSeconds = 1.0f / 60.0f) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = displaySize;
  io.DeltaTime = deltaSeconds;
  ImGui::NewFrame();
}

bool HasEventKind(const ReproFrame& frame, ReproEvent::Kind kind) {
  return std::any_of(frame.events.begin(), frame.events.end(),
                     [kind](const ReproEvent& event) { return event.kind == kind; });
}

void WriteTextFile(const std::filesystem::path& path, std::string_view text) {
  std::ofstream os(path);
  os << text;
}

std::string MetadataLineWith(std::string_view suffix) {
  std::string line = R"({"v":3,"svg":"foo.svg","wnd":[100,100],"scale":1.0,"exp":0)";
  line += suffix;
  line += "}\n";
  return line;
}

std::string FrameLineWith(std::string_view suffix) {
  std::string line = R"({"f":0,"t":0,"dt":16,"mx":1,"my":2,"btn":0,"mod":0)";
  line += suffix;
  line += "}\n";
  return line;
}

}  // namespace

TEST(ReproFileTest, RoundTripMetadataOnly) {
  const ReproFile orig = MakeFileWithOneFrame();
  const auto path = TempFile("meta_only");

  ASSERT_TRUE(WriteReproFile(path, orig));
  auto loaded = ReadReproFile(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->metadata.svgPath, "foo/bar.svg");
  EXPECT_EQ(loaded->metadata.windowWidth, 1600);
  EXPECT_EQ(loaded->metadata.windowHeight, 900);
  EXPECT_DOUBLE_EQ(loaded->metadata.displayScale, 2.0);
  EXPECT_TRUE(loaded->metadata.experimentalMode);
  EXPECT_EQ(loaded->metadata.startedAtIso8601, "2026-04-19T12:00:00Z");
  EXPECT_TRUE(loaded->metadata.svgBasename.empty());
  EXPECT_TRUE(loaded->metadata.svgContentHash.empty());
  EXPECT_FALSE(loaded->metadata.svgSource.has_value());
  ASSERT_EQ(loaded->frames.size(), 1u);
  EXPECT_DOUBLE_EQ(loaded->frames[0].mouseX, 123.25);
  EXPECT_DOUBLE_EQ(loaded->frames[0].mouseY, 456.75);
  EXPECT_FALSE(loaded->frames[0].mouseDocX.has_value());
  EXPECT_FALSE(loaded->frames[0].mouseDocY.has_value());
  EXPECT_FALSE(loaded->frames[0].viewport.has_value());
  EXPECT_FALSE(loaded->metadata.expect.has_value());

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RoundTripEmbeddedSvgSourceMetadata) {
  ReproFile file = MakeFileWithOneFrame();
  file.metadata.svgBasename = "input.svg";
  file.metadata.svgContentHash = "fnv1a64:0123456789abcdef";
  file.metadata.svgSource = "<svg viewBox=\"0 0 1 1\">\n<path d=\"M0 0\"/>\n</svg>";

  const auto path = TempFile("svg_source_metadata");
  ASSERT_TRUE(WriteReproFile(path, file));
  auto loaded = ReadReproFile(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->metadata.svgBasename, "input.svg");
  EXPECT_EQ(loaded->metadata.svgContentHash, "fnv1a64:0123456789abcdef");
  ASSERT_TRUE(loaded->metadata.svgSource.has_value());
  EXPECT_EQ(*loaded->metadata.svgSource, *file.metadata.svgSource);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, ReproRecorderEmbedsInitialSvgSource) {
  const auto path = TempFile("recorder_svg_source");

  ReproRecorderOptions options;
  options.outputPath = path;
  options.svgPath = "/tmp/donner/input.svg";
  options.svgSource = "<svg><path id=\"target\"/></svg>\n";
  ReproRecorder recorder(std::move(options));

  ASSERT_TRUE(recorder.flush());
  auto loaded = ReadReproFile(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->metadata.svgPath, "/tmp/donner/input.svg");
  EXPECT_EQ(loaded->metadata.svgBasename, "input.svg");
  EXPECT_TRUE(loaded->metadata.svgContentHash.starts_with("fnv1a64:"));
  ASSERT_TRUE(loaded->metadata.svgSource.has_value());
  EXPECT_EQ(*loaded->metadata.svgSource, "<svg><path id=\"target\"/></svg>\n");

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, ReproRecorderSnapshotsInputEdgesAndFrameContext) {
  ImGuiContextGuard imgui;
  const auto path = TempFile("recorder_input_edges");

  ReproRecorderOptions options;
  options.outputPath = path;
  options.svgPath = "/tmp/donner/input.svg";
  options.windowWidth = 640;
  options.windowHeight = 480;
  options.displayScale = 2.0;
  ReproRecorder recorder(std::move(options));

  ReproViewport viewport;
  viewport.zoom = 1.5;
  viewport.panDocX = 7.0;
  viewport.panDocY = 11.0;

  FrameContext context;
  context.viewport = viewport;
  context.mouseDoc = std::pair<double, double>{4.0, 8.0};
  context.hitTester = [](double docX, double docY) -> std::optional<ReproHit> {
    EXPECT_DOUBLE_EQ(docX, 4.0);
    EXPECT_DOUBLE_EQ(docY, 8.0);
    return ReproHit{.id = "target", .tag = "rect", .docOrderIndex = 3, .empty = false};
  };

  ImGuiIO& io = ImGui::GetIO();
  io.AddMousePosEvent(12.0f, 34.0f);
  BeginImGuiFrame(ImVec2(640.0f, 480.0f));
  recorder.snapshotFrame(context);
  ImGui::Render();

  io.AddMouseButtonEvent(0, true);
  io.AddMouseWheelEvent(1.25f, -2.5f);
  io.AddKeyEvent(ImGuiKey_LeftCtrl, true);
  io.AddKeyEvent(ImGuiKey_A, true);
  io.AddInputCharacter('x');
  io.AddFocusEvent(false);
  BeginImGuiFrame(ImVec2(800.0f, 600.0f));
  recorder.snapshotFrame(context);
  ImGui::Render();

  io.AddMouseButtonEvent(0, false);
  io.AddKeyEvent(ImGuiKey_A, false);
  io.AddKeyEvent(ImGuiKey_LeftCtrl, false);
  io.AddFocusEvent(true);
  BeginImGuiFrame(ImVec2(800.0f, 600.0f));
  recorder.snapshotFrame(context);
  ImGui::Render();

  ASSERT_EQ(recorder.frameCount(), 3u);
  ASSERT_TRUE(recorder.flush());

  auto loaded = ReadReproFile(path);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_EQ(loaded->frames.size(), 3u);

  const ReproFrame& baseline = loaded->frames[0];
  EXPECT_EQ(baseline.index, 0u);
  EXPECT_DOUBLE_EQ(baseline.mouseX, 12.0);
  EXPECT_DOUBLE_EQ(baseline.mouseY, 34.0);
  ASSERT_TRUE(baseline.viewport.has_value());
  EXPECT_DOUBLE_EQ(baseline.viewport->zoom, 1.5);
  EXPECT_DOUBLE_EQ(*baseline.mouseDocX, 4.0);
  EXPECT_DOUBLE_EQ(*baseline.mouseDocY, 8.0);

  const ReproFrame& inputFrame = loaded->frames[1];
  EXPECT_EQ(inputFrame.mouseButtonMask, 1);
  EXPECT_TRUE(HasEventKind(inputFrame, ReproEvent::Kind::MouseDown));
  EXPECT_TRUE(HasEventKind(inputFrame, ReproEvent::Kind::Resize));
  const auto mouseDown = std::find_if(
      inputFrame.events.begin(), inputFrame.events.end(),
      [](const ReproEvent& event) { return event.kind == ReproEvent::Kind::MouseDown; });
  ASSERT_NE(mouseDown, inputFrame.events.end());
  ASSERT_TRUE(mouseDown->hit.has_value());
  EXPECT_EQ(mouseDown->hit->id, "target");
  EXPECT_EQ(mouseDown->hit->tag, "rect");
  EXPECT_EQ(mouseDown->hit->docOrderIndex, 3);

  const auto resize =
      std::find_if(inputFrame.events.begin(), inputFrame.events.end(),
                   [](const ReproEvent& event) { return event.kind == ReproEvent::Kind::Resize; });
  ASSERT_NE(resize, inputFrame.events.end());
  EXPECT_EQ(resize->width, 800);
  EXPECT_EQ(resize->height, 600);

  EXPECT_EQ(loaded->frames[2].index, 2u);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RoundTripExpectationMetadata) {
  ReproFile file = MakeFileWithOneFrame();
  file.metadata.expect = ReproExpectation{
      .leftMouseDownOrdinal = 2,
      .frameOffsetAfterLeftMouseDown = 1,
      .minFrameIndex = 153,
      .maxFrameIndex = 156,
      .targetSelector = "#target",
      .cropMode = "document-canvas",
      .cropRect =
          ReproExpectedCrop{
              .x = 260,
              .y = 620,
              .width = 360,
              .height = 260,
          },
  };

  const auto path = TempFile("expect_metadata");
  ASSERT_TRUE(WriteReproFile(path, file));
  auto loaded = ReadReproFile(path);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_TRUE(loaded->metadata.expect.has_value());
  EXPECT_EQ(*loaded->metadata.expect, *file.metadata.expect);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RoundTripExpectationMetadataWithoutCrop) {
  ReproFile file = MakeFileWithOneFrame();
  file.metadata.expect = ReproExpectation{
      .leftMouseDownOrdinal = 2,
      .frameOffsetAfterLeftMouseDown = 0,
      .minFrameIndex = 203,
      .maxFrameIndex = 203,
      .targetSelector = "#Lightning_glow_dark",
      .cropMode = "document-canvas",
  };

  const auto path = TempFile("expect_metadata_no_crop");
  ASSERT_TRUE(WriteReproFile(path, file));
  auto loaded = ReadReproFile(path);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_TRUE(loaded->metadata.expect.has_value());
  EXPECT_EQ(*loaded->metadata.expect, *file.metadata.expect);
  EXPECT_FALSE(loaded->metadata.expect->cropRect.has_value());

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RoundTripExtendedExpectationMetadata) {
  ReproFile file = MakeFileWithOneFrame();
  file.metadata.expect = ReproExpectation{
      .proofKind = ReproExpectationProofKind::ActiveDragAlignment,
      .leftMouseDownOrdinal = 2,
      .frameOffsetAfterLeftMouseDown = 46,
      .minFrameIndex = 249,
      .maxFrameIndex = 250,
      .targetSelector = "#Lightning_glow_dark",
      .cropMode = "document-canvas",
      .activeFrameIndex = 249,
      .comparisonFrameIndex = 250,
      .expectedSelectionLabel = "<g> #Lightning_glow_dark",
      .statusStartFrameIndex = 153,
      .statusMaxFrameIndex = 333,
      .forbiddenStatusSubstring = "compositor not yet re-rasterized",
  };

  const auto path = TempFile("expect_metadata_extended");
  ASSERT_TRUE(WriteReproFile(path, file));
  auto loaded = ReadReproFile(path);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_TRUE(loaded->metadata.expect.has_value());
  EXPECT_EQ(*loaded->metadata.expect, *file.metadata.expect);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RoundTripSemanticActions) {
  ReproFile file = MakeFileWithOneFrame();
  file.frames[0].actions.push_back(ReproAction{
      .kind = ReproAction::Kind::SetActiveTool,
      .tool = "pen",
  });
  file.frames[0].actions.push_back(ReproAction{
      .kind = ReproAction::Kind::SetStyleProperty,
      .propertyName = "fill",
      .propertyValue = "#ff0000",
  });
  file.frames[0].actions.push_back(ReproAction{
      .kind = ReproAction::Kind::CommitPenPath,
  });

  const auto path = TempFile("semantic_actions");
  ASSERT_TRUE(WriteReproFile(path, file));
  auto loaded = ReadReproFile(path);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_EQ(loaded->frames.size(), 1u);
  EXPECT_EQ(loaded->frames[0].actions, file.frames[0].actions);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, ExpectationMetadataDefaultsProofKindForOldFixtures) {
  const auto path = TempFile("expect_metadata_old");
  {
    std::ofstream out(path);
    out << "{\"v\":2,\"svg\":\"foo.svg\",\"wnd\":[1600,900],\"scale\":2,\"exp\":0,"
           "\"expect\":{\"left_mouse_down_ordinal\":2,"
           "\"frame_offset_after_left_mouse_down\":1,\"min_frame_index\":153,"
           "\"max_frame_index\":156,\"target_selector\":\"#target\","
           "\"crop_mode\":\"document-canvas\"}}\n";
  }

  auto loaded = ReadReproFile(path);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_TRUE(loaded->metadata.expect.has_value());
  EXPECT_EQ(loaded->metadata.expect->proofKind, ReproExpectationProofKind::PresentedPixels);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, NewRecordingsDefaultLegacyExperimentalModeToFalse) {
  ReproFile file;
  EXPECT_FALSE(file.metadata.experimentalMode);

  ReproRecorderOptions options;
  EXPECT_FALSE(options.experimentalMode);
}

TEST(ReproFileTest, RoundTripWithAllEventKindsAndV2Fields) {
  ReproFile file = MakeFileWithOneFrame();

  ReproViewport viewport;
  viewport.paneOriginX = 560.0;
  viewport.paneOriginY = 22.0;
  viewport.paneSizeW = 1040.0;
  viewport.paneSizeH = 878.0;
  viewport.devicePixelRatio = 2.0;
  viewport.zoom = 1.5;
  viewport.panDocX = 446.0;
  viewport.panDocY = 256.0;
  viewport.panScreenX = 1080.0;
  viewport.panScreenY = 461.0;
  viewport.viewBoxX = 0.0;
  viewport.viewBoxY = 0.0;
  viewport.viewBoxW = 892.0;
  viewport.viewBoxH = 512.0;

  ReproFrame f1;
  f1.index = 1;
  f1.timestampSeconds = 0.016;
  f1.deltaMs = 16.5;
  f1.mouseX = 200.0;
  f1.mouseY = 100.0;
  f1.mouseDocX = 34.0;
  f1.mouseDocY = 12.0;
  f1.mouseButtonMask = 0b01;
  f1.modifiers = 0b0010;  // Shift
  f1.viewport = viewport;

  ReproEvent mdown;
  mdown.kind = ReproEvent::Kind::MouseDown;
  mdown.mouseButton = 0;
  ReproHit hit;
  hit.tag = "g";
  hit.id = "big_lightning_glow";
  hit.docOrderIndex = 42;
  mdown.hit = hit;
  f1.events.push_back(mdown);

  ReproEvent mup;
  mup.kind = ReproEvent::Kind::MouseUp;
  mup.mouseButton = 1;
  f1.events.push_back(mup);

  ReproEvent kdown;
  kdown.kind = ReproEvent::Kind::KeyDown;
  kdown.key = 542;  // Arbitrary ImGuiKey-ish value.
  kdown.modifiers = 0b0010;
  f1.events.push_back(kdown);

  ReproEvent kup;
  kup.kind = ReproEvent::Kind::KeyUp;
  kup.key = 542;
  kup.modifiers = 0;
  f1.events.push_back(kup);

  ReproEvent chr;
  chr.kind = ReproEvent::Kind::Char;
  chr.codepoint = 0x1F600;  // Emoji
  f1.events.push_back(chr);

  ReproEvent wheel;
  wheel.kind = ReproEvent::Kind::Wheel;
  wheel.wheelDeltaX = 0.5f;
  wheel.wheelDeltaY = -1.25f;
  f1.events.push_back(wheel);

  ReproEvent resize;
  resize.kind = ReproEvent::Kind::Resize;
  resize.width = 1920;
  resize.height = 1080;
  f1.events.push_back(resize);

  ReproEvent focus;
  focus.kind = ReproEvent::Kind::Focus;
  focus.focusOn = false;
  f1.events.push_back(focus);

  file.frames.push_back(f1);

  const auto path = TempFile("all_events");
  ASSERT_TRUE(WriteReproFile(path, file));
  auto loaded = ReadReproFile(path);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_EQ(loaded->frames.size(), 2u);
  ASSERT_TRUE(loaded->frames[1].mouseDocX.has_value());
  EXPECT_DOUBLE_EQ(*loaded->frames[1].mouseDocX, 34.0);
  ASSERT_TRUE(loaded->frames[1].mouseDocY.has_value());
  EXPECT_DOUBLE_EQ(*loaded->frames[1].mouseDocY, 12.0);
  ASSERT_TRUE(loaded->frames[1].viewport.has_value());
  EXPECT_DOUBLE_EQ(loaded->frames[1].viewport->paneOriginX, 560.0);
  EXPECT_DOUBLE_EQ(loaded->frames[1].viewport->zoom, 1.5);
  EXPECT_DOUBLE_EQ(loaded->frames[1].viewport->viewBoxW, 892.0);
  const auto& loadedEvents = loaded->frames[1].events;
  ASSERT_EQ(loadedEvents.size(), 8u);
  EXPECT_EQ(loadedEvents[0].kind, ReproEvent::Kind::MouseDown);
  EXPECT_EQ(loadedEvents[0].mouseButton, 0);
  ASSERT_TRUE(loadedEvents[0].hit.has_value());
  EXPECT_EQ(loadedEvents[0].hit->tag, "g");
  EXPECT_EQ(loadedEvents[0].hit->id, "big_lightning_glow");
  EXPECT_EQ(loadedEvents[0].hit->docOrderIndex, 42);
  EXPECT_FALSE(loadedEvents[0].hit->empty);
  EXPECT_EQ(loadedEvents[1].kind, ReproEvent::Kind::MouseUp);
  EXPECT_EQ(loadedEvents[1].mouseButton, 1);
  EXPECT_EQ(loadedEvents[2].kind, ReproEvent::Kind::KeyDown);
  EXPECT_EQ(loadedEvents[2].key, 542);
  EXPECT_EQ(loadedEvents[2].modifiers, 0b0010);
  EXPECT_EQ(loadedEvents[3].kind, ReproEvent::Kind::KeyUp);
  EXPECT_EQ(loadedEvents[3].key, 542);
  EXPECT_EQ(loadedEvents[4].kind, ReproEvent::Kind::Char);
  EXPECT_EQ(loadedEvents[4].codepoint, 0x1F600u);
  EXPECT_EQ(loadedEvents[5].kind, ReproEvent::Kind::Wheel);
  EXPECT_FLOAT_EQ(loadedEvents[5].wheelDeltaX, 0.5f);
  EXPECT_FLOAT_EQ(loadedEvents[5].wheelDeltaY, -1.25f);
  EXPECT_EQ(loadedEvents[6].kind, ReproEvent::Kind::Resize);
  EXPECT_EQ(loadedEvents[6].width, 1920);
  EXPECT_EQ(loadedEvents[6].height, 1080);
  EXPECT_EQ(loadedEvents[7].kind, ReproEvent::Kind::Focus);
  EXPECT_FALSE(loadedEvents[7].focusOn);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RoundTripEscapedStringsAndEmptyHit) {
  ReproFile file = MakeFileWithOneFrame();
  std::string escaped = "quote\" slash\\ back\b form\f line\n carriage\r tab\t ctrl";
  escaped.push_back('\x01');
  file.metadata.svgPath = escaped;
  file.metadata.svgBasename = "base\\name\".svg";
  file.metadata.svgSource = escaped;

  ReproFrame frame;
  frame.index = 2;
  frame.timestampSeconds = 1.5;
  frame.deltaMs = 8.0;
  frame.mouseX = 1.0;
  frame.mouseY = 2.0;
  frame.mouseButtonMask = 1;
  frame.modifiers = 4;
  ReproEvent mdown;
  mdown.kind = ReproEvent::Kind::MouseDown;
  mdown.mouseButton = 0;
  mdown.hit = ReproHit{.empty = true};
  frame.events.push_back(mdown);
  file.frames.push_back(frame);

  const auto path = TempFile("escaped_strings");
  ASSERT_TRUE(WriteReproFile(path, file));
  auto loaded = ReadReproFile(path);
  ASSERT_TRUE(loaded.has_value());

  EXPECT_EQ(loaded->metadata.svgPath, escaped);
  EXPECT_EQ(loaded->metadata.svgBasename, "base\\name\".svg");
  ASSERT_TRUE(loaded->metadata.svgSource.has_value());
  EXPECT_EQ(*loaded->metadata.svgSource, escaped);
  ASSERT_EQ(loaded->frames.size(), 2u);
  ASSERT_EQ(loaded->frames[1].events.size(), 1u);
  ASSERT_TRUE(loaded->frames[1].events[0].hit.has_value());
  EXPECT_TRUE(loaded->frames[1].events[0].hit->empty);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RoundTripTagOnlyHitFocusOnAndInvalidProofKindFallback) {
  ReproFile file = MakeFileWithOneFrame();
  file.metadata.expect = ReproExpectation{
      .proofKind = static_cast<ReproExpectationProofKind>(255),
      .leftMouseDownOrdinal = 1,
      .frameOffsetAfterLeftMouseDown = 0,
      .minFrameIndex = 1,
      .maxFrameIndex = 2,
      .targetSelector = "#target",
      .cropMode = "document-canvas",
  };

  ReproFrame frame;
  frame.index = 3;
  frame.timestampSeconds = 2.5;
  frame.deltaMs = 16.0;
  frame.mouseX = 8.0;
  frame.mouseY = 9.0;
  frame.mouseButtonMask = 0;
  frame.modifiers = 0;

  ReproEvent focus;
  focus.kind = ReproEvent::Kind::Focus;
  focus.focusOn = true;
  frame.events.push_back(focus);

  ReproEvent mouseDown;
  mouseDown.kind = ReproEvent::Kind::MouseDown;
  mouseDown.mouseButton = 0;
  mouseDown.hit = ReproHit{.tag = "path"};
  frame.events.push_back(mouseDown);
  file.frames.push_back(frame);

  const auto path = TempFile("tag_only_hit");
  ASSERT_TRUE(WriteReproFile(path, file));
  auto loaded = ReadReproFile(path);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_TRUE(loaded->metadata.expect.has_value());
  EXPECT_EQ(loaded->metadata.expect->proofKind, ReproExpectationProofKind::PresentedPixels);
  ASSERT_EQ(loaded->frames.back().events.size(), 2u);
  EXPECT_EQ(loaded->frames.back().events[0].kind, ReproEvent::Kind::Focus);
  EXPECT_TRUE(loaded->frames.back().events[0].focusOn);
  ASSERT_TRUE(loaded->frames.back().events[1].hit.has_value());
  EXPECT_EQ(loaded->frames.back().events[1].hit->tag, "path");
  EXPECT_TRUE(loaded->frames.back().events[1].hit->id.empty());
  EXPECT_EQ(loaded->frames.back().events[1].hit->docOrderIndex, -1);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, UnknownEventKindSerializedFromInvalidEnumIsRejectedOnRead) {
  ReproFile file = MakeFileWithOneFrame();
  ReproEvent event;
  event.kind = static_cast<ReproEvent::Kind>(255);
  file.frames.front().events.push_back(event);

  const auto path = TempFile("invalid_event_enum");
  ASSERT_TRUE(WriteReproFile(path, file));
  EXPECT_FALSE(ReadReproFile(path).has_value());

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, ReadsSignedExponentNumbersTabsAndBlankLines) {
  const auto path = TempFile("number_edges");
  WriteTextFile(path,
                "\n"
                "{\"v\":+3,\"svg\":\"foo.svg\",\"wnd\":[\t+1e2, +2E2],\"scale\":+1.5e0,"
                "\"exp\":+1}\n"
                "\n"
                "{\"f\":+0,\"t\":+1.25e0,\"dt\":+1.6E1,\"mx\":+1.25e1,\"my\":-2.5e0,"
                "\"btn\":+1,\"mod\":+2,\"e\":[{\"k\":\"focus\",\"on\":+1}]}\n"
                "\n");

  auto loaded = ReadReproFile(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->metadata.windowWidth, 100);
  EXPECT_EQ(loaded->metadata.windowHeight, 200);
  EXPECT_DOUBLE_EQ(loaded->metadata.displayScale, 1.5);
  EXPECT_TRUE(loaded->metadata.experimentalMode);
  ASSERT_EQ(loaded->frames.size(), 1u);
  EXPECT_DOUBLE_EQ(loaded->frames[0].timestampSeconds, 1.25);
  EXPECT_DOUBLE_EQ(loaded->frames[0].deltaMs, 16.0);
  EXPECT_DOUBLE_EQ(loaded->frames[0].mouseX, 12.5);
  EXPECT_DOUBLE_EQ(loaded->frames[0].mouseY, -2.5);
  ASSERT_EQ(loaded->frames[0].events.size(), 1u);
  EXPECT_TRUE(loaded->frames[0].events[0].focusOn);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RejectsOverflowingRequiredNumber) {
  const auto path = TempFile("overflow_number");
  WriteTextFile(path, R"({"v":1e999,"svg":"foo","wnd":[100,100],"scale":1.0,"exp":0})"
                      "\n");

  EXPECT_FALSE(ReadReproFile(path).has_value());

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, IgnoresMalformedOptionalEventFields) {
  const auto path = TempFile("optional_event_fields");
  WriteTextFile(path,
                MetadataLineWith("") +
                    FrameLineWith(R"(,"e":[{"k":"kdown","key":bad,"m":bad},)"
                                  R"({"k":"chr","c":bad},{"k":"wheel","dx":bad,"dy":bad},)"
                                  R"({"k":"focus","on":bad},)"
                                  R"({"k":"mdown","b":bad,"hit":{"empty":0,"tag":"path"}}])"));

  auto loaded = ReadReproFile(path);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_EQ(loaded->frames.size(), 1u);
  ASSERT_EQ(loaded->frames[0].events.size(), 5u);
  EXPECT_EQ(loaded->frames[0].events[0].key, 0);
  EXPECT_EQ(loaded->frames[0].events[0].modifiers, 0);
  EXPECT_EQ(loaded->frames[0].events[1].codepoint, 0u);
  EXPECT_FLOAT_EQ(loaded->frames[0].events[2].wheelDeltaX, 0.0f);
  EXPECT_FLOAT_EQ(loaded->frames[0].events[2].wheelDeltaY, 0.0f);
  EXPECT_TRUE(loaded->frames[0].events[3].focusOn);
  ASSERT_TRUE(loaded->frames[0].events[4].hit.has_value());
  EXPECT_FALSE(loaded->frames[0].events[4].hit->empty);
  EXPECT_EQ(loaded->frames[0].events[4].hit->tag, "path");

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, ReadMissingFileReturnsNullopt) {
  const auto path = TempFile("does_not_exist");

  EXPECT_FALSE(ReadReproFile(path).has_value());
}

TEST(ReproFileTest, ReadsV1FileWithV2FieldsDefaultConstructed) {
  const auto path = TempFile("v1_legacy");
  {
    std::ofstream os(path);
    os << R"({"v":1,"svg":"foo.svg","wnd":[1600,900],"scale":2.0,"exp":0})" << '\n';
    os << R"({"f":0,"t":0.0,"dt":16.6,"mx":100,"my":50,"btn":1,"mod":2,)"
       << R"("e":[{"k":"mdown","b":0},{"k":"wheel","dy":1.0}]})" << '\n';
  }

  auto loaded = ReadReproFile(path);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_EQ(loaded->frames.size(), 1u);
  const auto& frame = loaded->frames[0];
  EXPECT_DOUBLE_EQ(frame.mouseX, 100.0);
  EXPECT_DOUBLE_EQ(frame.mouseY, 50.0);
  EXPECT_EQ(frame.mouseButtonMask, 1);
  EXPECT_EQ(frame.modifiers, 2);
  EXPECT_FALSE(frame.mouseDocX.has_value());
  EXPECT_FALSE(frame.mouseDocY.has_value());
  EXPECT_FALSE(frame.viewport.has_value());
  ASSERT_EQ(frame.events.size(), 2u);
  EXPECT_FALSE(frame.events[0].hit.has_value());
  EXPECT_FALSE(frame.events[1].hit.has_value());

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RejectsVersionMismatch) {
  const auto path = TempFile("bad_version");
  {
    std::ofstream os(path);
    os << R"({"v":999,"svg":"foo","wnd":[100,100],"scale":1.0,"exp":0})" << '\n';
  }
  auto loaded = ReadReproFile(path);
  EXPECT_FALSE(loaded.has_value());
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RejectsFirstLineWithoutVersion) {
  const auto path = TempFile("missing_version");
  WriteTextFile(path, R"({"svg":"foo","wnd":[100,100],"scale":1.0,"exp":0})"
                      "\n");

  EXPECT_FALSE(ReadReproFile(path).has_value());

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RejectsNonNumericVersion) {
  const auto path = TempFile("bad_numeric_version");
  WriteTextFile(path, R"({"v":"bad","svg":"foo","wnd":[100,100],"scale":1.0,"exp":0})"
                      "\n");

  EXPECT_FALSE(ReadReproFile(path).has_value());

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RejectsMissingMetadata) {
  const auto path = TempFile("no_meta");
  { std::ofstream os(path); }
  auto loaded = ReadReproFile(path);
  EXPECT_FALSE(loaded.has_value());
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RejectsFrameWithOnlyOneDocumentCoordinate) {
  const auto path = TempFile("bad_doc_pair");
  WriteTextFile(path, R"({"v":3,"svg":"foo.svg","wnd":[100,100],"scale":1.0,"exp":0})"
                      "\n"
                      R"({"f":0,"t":0,"dt":16,"mx":1,"my":2,"btn":0,"mod":0,"mdx":3})"
                      "\n");

  EXPECT_FALSE(ReadReproFile(path).has_value());

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RejectsMalformedViewportBlock) {
  const auto path = TempFile("bad_viewport");
  WriteTextFile(path, R"({"v":3,"svg":"foo.svg","wnd":[100,100],"scale":1.0,"exp":0})"
                      "\n"
                      R"({"f":0,"t":0,"dt":16,"mx":1,"my":2,"btn":0,"mod":0,)"
                      R"("vp":{"ox":0,"oy":0,"pw":10,"ph":20,"dpr":1,"z":1,)"
                      R"("pdx":0,"pdy":0,"psx":0,"psy":0,"vbx":0,"vby":0,"vbw":10}})"
                      "\n");

  EXPECT_FALSE(ReadReproFile(path).has_value());

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RejectsMalformedEventArrayAndUnknownEventKind) {
  const auto missingArrayPath = TempFile("bad_event_array");
  WriteTextFile(missingArrayPath, R"({"v":3,"svg":"foo.svg","wnd":[100,100],"scale":1.0,"exp":0})"
                                  "\n"
                                  R"({"f":0,"t":0,"dt":16,"mx":1,"my":2,"btn":0,"mod":0,"e":{}})"
                                  "\n");
  EXPECT_FALSE(ReadReproFile(missingArrayPath).has_value());

  const auto unknownKindPath = TempFile("bad_event_kind");
  WriteTextFile(unknownKindPath,
                R"({"v":3,"svg":"foo.svg","wnd":[100,100],"scale":1.0,"exp":0})"
                "\n"
                R"({"f":0,"t":0,"dt":16,"mx":1,"my":2,"btn":0,"mod":0,"e":[{"k":"bogus"}]})"
                "\n");
  EXPECT_FALSE(ReadReproFile(unknownKindPath).has_value());

  std::error_code ec;
  std::filesystem::remove(missingArrayPath, ec);
  std::filesystem::remove(unknownKindPath, ec);
}

TEST(ReproFileTest, RejectsMalformedHitAndExpectationBlocks) {
  const auto badHitPath = TempFile("bad_hit");
  WriteTextFile(badHitPath, R"({"v":3,"svg":"foo.svg","wnd":[100,100],"scale":1.0,"exp":0})"
                            "\n"
                            R"({"f":0,"t":0,"dt":16,"mx":1,"my":2,"btn":0,"mod":0,)"
                            R"("e":[{"k":"mdown","b":0,"hit":{"idx":"bad"}}]})"
                            "\n");
  EXPECT_FALSE(ReadReproFile(badHitPath).has_value());

  const auto badExpectPath = TempFile("bad_expect");
  WriteTextFile(badExpectPath,
                R"({"v":3,"svg":"foo.svg","wnd":[100,100],"scale":1.0,"exp":0,)"
                R"("expect":{"proof_kind":"unknown","left_mouse_down_ordinal":1,)"
                R"("frame_offset_after_left_mouse_down":0,"min_frame_index":0,)"
                R"("max_frame_index":0,"target_selector":"#x","crop_mode":"document"}})"
                "\n");
  EXPECT_FALSE(ReadReproFile(badExpectPath).has_value());

  std::error_code ec;
  std::filesystem::remove(badHitPath, ec);
  std::filesystem::remove(badExpectPath, ec);
}

TEST(ReproFileTest, RoundTripsSelectionAndWorkerLivenessExpectationKinds) {
  for (ReproExpectationProofKind proofKind :
       {ReproExpectationProofKind::Selection, ReproExpectationProofKind::WorkerLiveness}) {
    ReproFile file = MakeFileWithOneFrame();
    file.metadata.expect = ReproExpectation{
        .proofKind = proofKind,
        .leftMouseDownOrdinal = 1,
        .frameOffsetAfterLeftMouseDown = 0,
        .minFrameIndex = 4,
        .maxFrameIndex = 8,
        .targetSelector = "#target",
        .cropMode = "document-canvas",
    };

    const auto path = TempFile("expect_kind");
    ASSERT_TRUE(WriteReproFile(path, file));
    auto loaded = ReadReproFile(path);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_TRUE(loaded->metadata.expect.has_value());
    EXPECT_EQ(loaded->metadata.expect->proofKind, proofKind);

    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}

TEST(ReproFileTest, RejectsMalformedEscapesInExpectationStrings) {
  const auto path = TempFile("bad_escape");
  WriteTextFile(path, R"({"v":3,"svg":"foo.svg","wnd":[100,100],"scale":1.0,"exp":0,)"
                      R"("expect":{"left_mouse_down_ordinal":1,)"
                      R"("frame_offset_after_left_mouse_down":0,"min_frame_index":0,)"
                      R"("max_frame_index":0,"target_selector":"bad \u12xz",)"
                      R"("crop_mode":"document"}})"
                      "\n");

  EXPECT_FALSE(ReadReproFile(path).has_value());

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RejectsIncompleteExpectationRequiredFieldsAndCrop) {
  const auto missingFieldPath = TempFile("expect_missing_required");
  WriteTextFile(missingFieldPath, R"({"v":3,"svg":"foo.svg","wnd":[100,100],"scale":1.0,"exp":0,)"
                                  R"("expect":{"left_mouse_down_ordinal":1,)"
                                  R"("frame_offset_after_left_mouse_down":0,"min_frame_index":0,)"
                                  R"("target_selector":"#x","crop_mode":"document"}})"
                                  "\n");
  EXPECT_FALSE(ReadReproFile(missingFieldPath).has_value());

  const auto malformedCropPath = TempFile("expect_bad_crop");
  WriteTextFile(malformedCropPath,
                R"({"v":3,"svg":"foo.svg","wnd":[100,100],"scale":1.0,"exp":0,)"
                R"("expect":{"left_mouse_down_ordinal":1,)"
                R"("frame_offset_after_left_mouse_down":0,"min_frame_index":0,)"
                R"("max_frame_index":0,"target_selector":"#x","crop_mode":"document",)"
                R"("crop":{"x":1,"y":2,"w":3}}})"
                "\n");
  EXPECT_FALSE(ReadReproFile(malformedCropPath).has_value());

  std::error_code ec;
  std::filesystem::remove(missingFieldPath, ec);
  std::filesystem::remove(malformedCropPath, ec);
}

TEST(ReproFileTest, RejectsMalformedFrameNumbersAndUnbalancedEventObject) {
  const auto badNumberPath = TempFile("frame_bad_number");
  WriteTextFile(badNumberPath, R"({"v":3,"svg":"foo.svg","wnd":[100,100],"scale":1.0,"exp":0})"
                               "\n"
                               R"({"f":0,"t":0,"dt":16,"mx":bad,"my":2,"btn":0,"mod":0})"
                               "\n");
  EXPECT_FALSE(ReadReproFile(badNumberPath).has_value());

  const auto unbalancedEventPath = TempFile("event_unbalanced");
  WriteTextFile(unbalancedEventPath,
                R"({"v":3,"svg":"foo.svg","wnd":[100,100],"scale":1.0,"exp":0})"
                "\n"
                R"({"f":0,"t":0,"dt":16,"mx":1,"my":2,"btn":0,"mod":0,"e":[{"k":"mdown")"
                "\n");
  EXPECT_FALSE(ReadReproFile(unbalancedEventPath).has_value());

  std::error_code ec;
  std::filesystem::remove(badNumberPath, ec);
  std::filesystem::remove(unbalancedEventPath, ec);
}

TEST(ReproFileTest, ReadsUnicodeEscapesAndIgnoresMalformedOptionalMetadata) {
  const auto path = TempFile("unicode_metadata");
  WriteTextFile(path, R"({"v":3,"svg":"caf\u00e9 \u20ac","wnd":[bad],)"
                      R"("scale":bad,"exp":bad,"at":123,"svg_base":123,)"
                      R"("svg_hash":123,"svg_src":123})"
                      "\n" +
                          FrameLineWith(""));

  auto loaded = ReadReproFile(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->metadata.svgPath, std::string("caf\xc3\xa9 \xe2\x82\xac"));
  EXPECT_EQ(loaded->metadata.windowWidth, 0);
  EXPECT_EQ(loaded->metadata.windowHeight, 0);
  EXPECT_DOUBLE_EQ(loaded->metadata.displayScale, 1.0);
  EXPECT_FALSE(loaded->metadata.experimentalMode);
  EXPECT_TRUE(loaded->metadata.startedAtIso8601.empty());
  EXPECT_TRUE(loaded->metadata.svgBasename.empty());
  EXPECT_TRUE(loaded->metadata.svgContentHash.empty());
  EXPECT_FALSE(loaded->metadata.svgSource.has_value());

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RejectsMalformedRequiredFrameFields) {
  const struct {
    std::string_view label;
    std::string_view frameLine;
  } cases[] = {
      {"missing_frame_index", R"({"t":0,"dt":16,"mx":1,"my":2,"btn":0,"mod":0})"},
      {"missing_timestamp", R"({"f":0,"dt":16,"mx":1,"my":2,"btn":0,"mod":0})"},
      {"missing_delta", R"({"f":0,"t":0,"mx":1,"my":2,"btn":0,"mod":0})"},
      {"missing_mouse_x", R"({"f":0,"t":0,"dt":16,"my":2,"btn":0,"mod":0})"},
      {"missing_mouse_y", R"({"f":0,"t":0,"dt":16,"mx":1,"btn":0,"mod":0})"},
      {"missing_buttons", R"({"f":0,"t":0,"dt":16,"mx":1,"my":2,"mod":0})"},
      {"missing_modifiers", R"({"f":0,"t":0,"dt":16,"mx":1,"my":2,"btn":0})"},
      {"bad_modifiers", R"({"f":0,"t":0,"dt":16,"mx":1,"my":2,"btn":0,"mod":bad})"},
  };

  for (const auto& testCase : cases) {
    SCOPED_TRACE(testCase.label);
    const auto path = TempFile(testCase.label);
    WriteTextFile(path, MetadataLineWith("") + std::string(testCase.frameLine) + "\n");

    EXPECT_FALSE(ReadReproFile(path).has_value());

    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}

TEST(ReproFileTest, RejectsMalformedViewportVariants) {
  const struct {
    std::string_view label;
    std::string_view viewportBody;
  } cases[] = {
      {"missing_ox",
       R"("oy":0,"pw":10,"ph":20,"dpr":1,"z":1,"pdx":0,"pdy":0,"psx":0,"psy":0,"vbx":0,"vby":0,"vbw":10,"vbh":20)"},
      {"missing_oy",
       R"("ox":0,"pw":10,"ph":20,"dpr":1,"z":1,"pdx":0,"pdy":0,"psx":0,"psy":0,"vbx":0,"vby":0,"vbw":10,"vbh":20)"},
      {"missing_pw",
       R"("ox":0,"oy":0,"ph":20,"dpr":1,"z":1,"pdx":0,"pdy":0,"psx":0,"psy":0,"vbx":0,"vby":0,"vbw":10,"vbh":20)"},
      {"missing_ph",
       R"("ox":0,"oy":0,"pw":10,"dpr":1,"z":1,"pdx":0,"pdy":0,"psx":0,"psy":0,"vbx":0,"vby":0,"vbw":10,"vbh":20)"},
      {"missing_dpr",
       R"("ox":0,"oy":0,"pw":10,"ph":20,"z":1,"pdx":0,"pdy":0,"psx":0,"psy":0,"vbx":0,"vby":0,"vbw":10,"vbh":20)"},
      {"missing_zoom",
       R"("ox":0,"oy":0,"pw":10,"ph":20,"dpr":1,"pdx":0,"pdy":0,"psx":0,"psy":0,"vbx":0,"vby":0,"vbw":10,"vbh":20)"},
      {"missing_pan_doc_x",
       R"("ox":0,"oy":0,"pw":10,"ph":20,"dpr":1,"z":1,"pdy":0,"psx":0,"psy":0,"vbx":0,"vby":0,"vbw":10,"vbh":20)"},
      {"missing_pan_doc_y",
       R"("ox":0,"oy":0,"pw":10,"ph":20,"dpr":1,"z":1,"pdx":0,"psx":0,"psy":0,"vbx":0,"vby":0,"vbw":10,"vbh":20)"},
      {"missing_pan_screen_x",
       R"("ox":0,"oy":0,"pw":10,"ph":20,"dpr":1,"z":1,"pdx":0,"pdy":0,"psy":0,"vbx":0,"vby":0,"vbw":10,"vbh":20)"},
      {"missing_pan_screen_y",
       R"("ox":0,"oy":0,"pw":10,"ph":20,"dpr":1,"z":1,"pdx":0,"pdy":0,"psx":0,"vbx":0,"vby":0,"vbw":10,"vbh":20)"},
      {"missing_view_box_x",
       R"("ox":0,"oy":0,"pw":10,"ph":20,"dpr":1,"z":1,"pdx":0,"pdy":0,"psx":0,"psy":0,"vby":0,"vbw":10,"vbh":20)"},
      {"missing_view_box_y",
       R"("ox":0,"oy":0,"pw":10,"ph":20,"dpr":1,"z":1,"pdx":0,"pdy":0,"psx":0,"psy":0,"vbx":0,"vbw":10,"vbh":20)"},
      {"missing_view_box_w",
       R"("ox":0,"oy":0,"pw":10,"ph":20,"dpr":1,"z":1,"pdx":0,"pdy":0,"psx":0,"psy":0,"vbx":0,"vby":0,"vbh":20)"},
      {"bad_origin_x",
       R"("ox":bad,"oy":0,"pw":10,"ph":20,"dpr":1,"z":1,"pdx":0,"pdy":0,"psx":0,"psy":0,"vbx":0,"vby":0,"vbw":10,"vbh":20)"},
  };

  for (const auto& testCase : cases) {
    SCOPED_TRACE(testCase.label);
    const auto path = TempFile(testCase.label);
    WriteTextFile(path,
                  MetadataLineWith("") + FrameLineWith(std::string(",\"vp\":{") +
                                                       std::string(testCase.viewportBody) + "}"));

    EXPECT_FALSE(ReadReproFile(path).has_value());

    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}

TEST(ReproFileTest, RejectsMalformedEventAndHitVariants) {
  const struct {
    std::string_view label;
    std::string_view eventSuffix;
  } cases[] = {
      {"event_missing_kind", R"(,"e":[{}])"},
      {"event_kind_not_string", R"(,"e":[{"k":123}])"},
      {"event_array_non_object", R"(,"e":[42])"},
      {"hit_not_object", R"(,"e":[{"k":"mdown","hit":[]}])"},
      {"hit_empty_not_number", R"(,"e":[{"k":"mdown","hit":{"empty":"bad"}}])"},
      {"hit_tag_not_string", R"(,"e":[{"k":"mdown","hit":{"tag":123}}])"},
      {"hit_id_not_string", R"(,"e":[{"k":"mdown","hit":{"id":123}}])"},
  };

  for (const auto& testCase : cases) {
    SCOPED_TRACE(testCase.label);
    const auto path = TempFile(testCase.label);
    WriteTextFile(path, MetadataLineWith("") + FrameLineWith(testCase.eventSuffix));

    EXPECT_FALSE(ReadReproFile(path).has_value());

    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}

TEST(ReproFileTest, RejectsMalformedExpectationVariants) {
  const std::string required =
      R"("left_mouse_down_ordinal":1,"frame_offset_after_left_mouse_down":0,)"
      R"("min_frame_index":0,"max_frame_index":1,"target_selector":"#x",)"
      R"("crop_mode":"document")";
  const struct {
    std::string_view label;
    std::string expectBody;
  } cases[] = {
      {"proof_kind_not_string", std::string(R"("proof_kind":123,)") + required},
      {"left_ordinal_missing",
       R"("frame_offset_after_left_mouse_down":0,"min_frame_index":0,)"
       R"("max_frame_index":1,"target_selector":"#x","crop_mode":"document")"},
      {"offset_missing", R"("left_mouse_down_ordinal":1,"min_frame_index":0,"max_frame_index":1,)"
                         R"("target_selector":"#x","crop_mode":"document")"},
      {"min_frame_missing", R"("left_mouse_down_ordinal":1,"frame_offset_after_left_mouse_down":0,)"
                            R"("max_frame_index":1,"target_selector":"#x","crop_mode":"document")"},
      {"target_selector_missing",
       R"("left_mouse_down_ordinal":1,"frame_offset_after_left_mouse_down":0,)"
       R"("min_frame_index":0,"max_frame_index":1,"crop_mode":"document")"},
      {"crop_mode_missing", R"("left_mouse_down_ordinal":1,"frame_offset_after_left_mouse_down":0,)"
                            R"("min_frame_index":0,"max_frame_index":1,"target_selector":"#x")"},
      {"target_selector_unclosed",
       R"("left_mouse_down_ordinal":1,"frame_offset_after_left_mouse_down":0,)"
       R"("min_frame_index":0,"max_frame_index":1,"target_selector":"#x,)"
       R"("crop_mode":"document")"},
      {"target_selector_bad_short_unicode",
       R"("left_mouse_down_ordinal":1,"frame_offset_after_left_mouse_down":0,)"
       R"("min_frame_index":0,"max_frame_index":1,"target_selector":"\u12",)"
       R"("crop_mode":"document")"},
      {"target_selector_unknown_escape",
       R"("left_mouse_down_ordinal":1,"frame_offset_after_left_mouse_down":0,)"
       R"("min_frame_index":0,"max_frame_index":1,"target_selector":"#\q",)"
       R"("crop_mode":"document")"},
      {"crop_not_object", std::string(required) + R"(,"crop":[])"},
      {"crop_x_missing", std::string(required) + R"(,"crop":{"y":2,"w":3,"h":4})"},
      {"crop_y_missing", std::string(required) + R"(,"crop":{"x":1,"w":3,"h":4})"},
      {"crop_w_missing", std::string(required) + R"(,"crop":{"x":1,"y":2,"h":4})"},
      {"crop_x_bad", std::string(required) + R"(,"crop":{"x":bad,"y":2,"w":3,"h":4})"},
      {"active_frame_bad", std::string(required) + R"(,"active_frame_index":"bad")"},
      {"comparison_frame_bad", std::string(required) + R"(,"comparison_frame_index":"bad")"},
      {"selection_label_bad", std::string(required) + R"(,"expected_selection_label":123)"},
      {"status_start_bad", std::string(required) + R"(,"status_start_frame_index":"bad")"},
      {"status_max_bad", std::string(required) + R"(,"status_max_frame_index":"bad")"},
      {"forbidden_status_bad", std::string(required) + R"(,"forbidden_status_substring":123)"},
  };

  for (const auto& testCase : cases) {
    SCOPED_TRACE(testCase.label);
    const auto path = TempFile(testCase.label);
    WriteTextFile(path,
                  MetadataLineWith(std::string(R"(,"expect":{)") + testCase.expectBody + "}"));

    if (testCase.label == std::string_view("target_selector_unknown_escape")) {
      auto loaded = ReadReproFile(path);
      ASSERT_TRUE(loaded.has_value());
      ASSERT_TRUE(loaded->metadata.expect.has_value());
      EXPECT_EQ(loaded->metadata.expect->targetSelector, "#q");
    } else {
      EXPECT_FALSE(ReadReproFile(path).has_value());
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}

TEST(ReproFileTest, WriteFailureWhenDestinationIsDirectory) {
  const auto path = TempFile("directory_destination");
  std::filesystem::create_directory(path);

  EXPECT_FALSE(WriteReproFile(path, MakeFileWithOneFrame()));

  std::error_code ec;
  std::filesystem::remove(path.string() + ".tmp", ec);
  std::filesystem::remove(path, ec);
}

}  // namespace donner::editor::repro
