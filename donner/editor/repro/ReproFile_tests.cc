#include "donner/editor/repro/ReproFile.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

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
  ASSERT_EQ(loaded->frames.size(), 1u);
  EXPECT_DOUBLE_EQ(loaded->frames[0].mouseX, 123.25);
  EXPECT_DOUBLE_EQ(loaded->frames[0].mouseY, 456.75);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RoundTripWithAllEventKinds) {
  ReproFile file = MakeFileWithOneFrame();

  ReproFrame f1;
  f1.index = 1;
  f1.timestampSeconds = 0.016;
  f1.deltaMs = 16.5;
  f1.mouseX = 200.0;
  f1.mouseY = 100.0;
  f1.mouseButtonMask = 0b01;
  f1.modifiers = 0b0010;  // Shift

  ReproEvent mdown;
  mdown.kind = ReproEvent::Kind::MouseDown;
  mdown.mouseButton = 0;
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
  const auto& loadedEvents = loaded->frames[1].events;
  ASSERT_EQ(loadedEvents.size(), 8u);
  EXPECT_EQ(loadedEvents[0].kind, ReproEvent::Kind::MouseDown);
  EXPECT_EQ(loadedEvents[0].mouseButton, 0);
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

TEST(ReproFileTest, RejectsV1WithRerecordDiagnostic) {
  // v1 recordings had no viewport snapshot, so the replayer had to
  // reconstruct screen→doc math from hand-tuned pane-layout constants.
  // That silently routed clicks to the wrong element whenever the live
  // ImGui layout drifted. We reject v1 now — the loader tells the user
  // to rerecord rather than producing a silently-divergent replay.
  const auto path = TempFile("v1_legacy");
  {
    std::ofstream os(path);
    os << R"({"v":1,"svg":"foo.svg","wnd":[1600,900],"scale":2.0,"exp":0})" << '\n';
    os << R"({"f":0,"t":0.0,"dt":16.6,"mx":100,"my":50,"btn":0,"mod":0})" << '\n';
  }
  auto loaded = ReadReproFile(path);
  EXPECT_FALSE(loaded.has_value());
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RoundTripViewportAndDocCoords) {
  ReproFile file = MakeFileWithOneFrame();
  // Attach a viewport to frame 0.
  ReproViewport vp;
  vp.paneOriginX = 560.0;
  vp.paneOriginY = 22.0;
  vp.paneSizeW = 1040.0;
  vp.paneSizeH = 878.0;
  vp.devicePixelRatio = 2.0;
  vp.zoom = 1.5;
  vp.panDocX = 446.0;
  vp.panDocY = 256.0;
  vp.panScreenX = 1080.0;
  vp.panScreenY = 461.0;
  vp.viewBoxX = 0.0;
  vp.viewBoxY = 0.0;
  vp.viewBoxW = 892.0;
  vp.viewBoxH = 512.0;
  file.frames[0].viewport = vp;
  file.frames[0].mouseDocX = 12.5;
  file.frames[0].mouseDocY = -3.25;

  // Frame 1: same viewport (writer should delta-encode and omit `vp`,
  // but the reader carries the previous `vp` forward so readers see
  // it on every frame).
  ReproFrame f1;
  f1.index = 1;
  f1.timestampSeconds = 0.016;
  f1.deltaMs = 16.5;
  f1.mouseX = 200.0;
  f1.mouseY = 100.0;
  f1.mouseDocX = 34.0;
  f1.mouseDocY = 12.0;
  f1.mouseButtonMask = 1;
  f1.modifiers = 0;
  // Emit the viewport ourselves — the writer preserves whatever the
  // in-memory structure says. (In practice the recorder delta-encodes;
  // this test is documenting round-trip regardless of delta choice.)
  f1.viewport = vp;
  file.frames.push_back(f1);

  const auto path = TempFile("viewport_round_trip");
  ASSERT_TRUE(WriteReproFile(path, file));
  auto loaded = ReadReproFile(path);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_EQ(loaded->frames.size(), 2u);

  ASSERT_TRUE(loaded->frames[0].viewport.has_value());
  EXPECT_DOUBLE_EQ(loaded->frames[0].viewport->paneOriginX, 560.0);
  EXPECT_DOUBLE_EQ(loaded->frames[0].viewport->zoom, 1.5);
  EXPECT_DOUBLE_EQ(loaded->frames[0].viewport->viewBoxW, 892.0);
  ASSERT_TRUE(loaded->frames[0].mouseDocX.has_value());
  EXPECT_DOUBLE_EQ(*loaded->frames[0].mouseDocX, 12.5);
  EXPECT_DOUBLE_EQ(*loaded->frames[0].mouseDocY, -3.25);

  // Frame 1 viewport should match (carried forward or emitted).
  ASSERT_TRUE(loaded->frames[1].viewport.has_value());
  EXPECT_DOUBLE_EQ(loaded->frames[1].viewport->zoom, 1.5);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, ViewportDeltaCarriedForwardInReader) {
  // Hand-build a file where frame 0 has a `vp` block but frame 1
  // doesn't — the reader must carry the viewport forward so downstream
  // consumers see a populated `frame.viewport` on every frame.
  const auto path = TempFile("viewport_delta");
  {
    std::ofstream os(path);
    os << R"({"v":2,"svg":"foo.svg","wnd":[1600,900],"scale":2.0,"exp":0})" << '\n';
    os << R"({"f":0,"t":0,"dt":16,"mx":0,"my":0,"btn":0,"mod":0,)"
       << R"("vp":{"ox":10,"oy":20,"pw":100,"ph":200,"dpr":1,"z":1,)"
       << R"("pdx":0,"pdy":0,"psx":50,"psy":100,)"
       << R"("vbx":0,"vby":0,"vbw":100,"vbh":200}})" << '\n';
    os << R"({"f":1,"t":0.016,"dt":16,"mx":10,"my":20,"btn":0,"mod":0})" << '\n';
  }
  auto loaded = ReadReproFile(path);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_EQ(loaded->frames.size(), 2u);
  ASSERT_TRUE(loaded->frames[1].viewport.has_value());
  EXPECT_DOUBLE_EQ(loaded->frames[1].viewport->paneOriginX, 10.0);
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(ReproFileTest, RoundTripsMouseDownHitCheckpoint) {
  ReproFile file = MakeFileWithOneFrame();

  ReproFrame f1;
  f1.index = 1;
  f1.timestampSeconds = 0.016;
  f1.deltaMs = 16.5;
  f1.mouseX = 200.0;
  f1.mouseY = 100.0;
  f1.mouseButtonMask = 1;

  ReproEvent mdown;
  mdown.kind = ReproEvent::Kind::MouseDown;
  mdown.mouseButton = 0;
  ReproHit hit;
  hit.tag = "g";
  hit.id = "big_lightning_glow";
  hit.docOrderIndex = 42;
  mdown.hit = hit;
  f1.events.push_back(mdown);

  ReproEvent mdownEmpty;
  mdownEmpty.kind = ReproEvent::Kind::MouseDown;
  mdownEmpty.mouseButton = 0;
  ReproHit emptyHit;
  emptyHit.empty = true;
  mdownEmpty.hit = emptyHit;
  f1.events.push_back(mdownEmpty);

  file.frames.push_back(f1);

  const auto path = TempFile("hit_checkpoint");
  ASSERT_TRUE(WriteReproFile(path, file));
  auto loaded = ReadReproFile(path);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_EQ(loaded->frames.size(), 2u);
  const auto& evs = loaded->frames[1].events;
  ASSERT_EQ(evs.size(), 2u);
  ASSERT_TRUE(evs[0].hit.has_value());
  EXPECT_EQ(evs[0].hit->tag, "g");
  EXPECT_EQ(evs[0].hit->id, "big_lightning_glow");
  EXPECT_EQ(evs[0].hit->docOrderIndex, 42);
  EXPECT_FALSE(evs[0].hit->empty);
  ASSERT_TRUE(evs[1].hit.has_value());
  EXPECT_TRUE(evs[1].hit->empty);

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

}  // namespace donner::editor::repro
