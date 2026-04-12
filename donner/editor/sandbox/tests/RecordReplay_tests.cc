/// @file
///
/// Tests for S4 record/replay and the headless frame inspector. These
/// cover three things:
///
/// 1. **`.rnr` file round-trip** — encode + decode header, handle truncation,
///    bad magic, and version mismatches.
/// 2. **`FrameInspector::Decode`** — parses a serialized frame and produces a
///    sensible command list (indices, depth tracking, summaries that aren't
///    empty for supported opcodes).
/// 3. **Pixel round-trip through a `.rnr` file** — serialize → save → load →
///    replay → compare against an in-process RendererTinySkia render, and
///    verify the result is byte-identical.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/editor/sandbox/FrameInspector.h"
#include "donner/editor/sandbox/ReplayingRenderer.h"
#include "donner/editor/sandbox/RnrFile.h"
#include "donner/editor/sandbox/SerializingRenderer.h"
#include "donner/editor/sandbox/Wire.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/RendererTinySkia.h"

namespace donner::editor::sandbox {
namespace {

constexpr std::string_view kSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
       <g>
         <rect width="64" height="64" fill="red"/>
         <rect x="16" y="16" width="32" height="32" fill="blue"/>
       </g>
     </svg>)";

svg::SVGDocument ParseOrDie(std::string_view svg) {
  ParseWarningSink warnings;
  auto result = svg::parser::SVGParser::ParseSVG(svg, warnings);
  EXPECT_FALSE(result.hasError()) << result.error();
  return std::move(result.result());
}

std::vector<uint8_t> SerializeSimpleFrame(std::string_view svg, int w, int h) {
  auto doc = ParseOrDie(svg);
  doc.setCanvasSize(w, h);
  SerializingRenderer serializer;
  serializer.draw(doc);
  return std::move(serializer).takeBuffer();
}

svg::RendererBitmap RenderInProcess(std::string_view svg, int w, int h) {
  auto doc = ParseOrDie(svg);
  doc.setCanvasSize(w, h);
  svg::RendererTinySkia renderer;
  renderer.draw(doc);
  return renderer.takeSnapshot();
}

// -----------------------------------------------------------------------------
// RnrFile round-trip
// -----------------------------------------------------------------------------

TEST(RnrFileTest, EncodeDecodeMemoryBufferRoundTrips) {
  RnrHeader header;
  header.timestampNanos = 1744487136ull * 1'000'000'000ull;
  header.width = 640;
  header.height = 480;
  header.backend = BackendHint::kTinySkia;
  header.uri = "file:///tmp/test.svg";

  const std::vector<uint8_t> wire = {0x01, 0x02, 0x03, 0x04, 0xDE, 0xAD, 0xBE, 0xEF};

  const auto encoded = EncodeRnrBuffer(header, wire);

  RnrHeader decodedHeader;
  std::vector<uint8_t> decodedWire;
  ASSERT_EQ(ParseRnrBuffer(encoded, decodedHeader, decodedWire), RnrIoStatus::kOk);
  EXPECT_EQ(decodedHeader.fileVersion, kRnrFileVersion);
  EXPECT_EQ(decodedHeader.timestampNanos, header.timestampNanos);
  EXPECT_EQ(decodedHeader.width, header.width);
  EXPECT_EQ(decodedHeader.height, header.height);
  EXPECT_EQ(decodedHeader.backend, BackendHint::kTinySkia);
  EXPECT_EQ(decodedHeader.uri, header.uri);
  EXPECT_EQ(decodedWire, wire);
}

TEST(RnrFileTest, TruncatedHeaderReturnsTruncated) {
  std::vector<uint8_t> buf = {0x44, 0x52, 0x4E, 0x46};  // only magic
  RnrHeader header;
  std::vector<uint8_t> wire;
  EXPECT_EQ(ParseRnrBuffer(buf, header, wire), RnrIoStatus::kTruncated);
}

TEST(RnrFileTest, WrongMagicRejected) {
  std::vector<uint8_t> buf(32, 0);
  RnrHeader header;
  std::vector<uint8_t> wire;
  EXPECT_EQ(ParseRnrBuffer(buf, header, wire), RnrIoStatus::kMagicMismatch);
}

TEST(RnrFileTest, VersionMismatchRejected) {
  RnrHeader header;
  header.fileVersion = kRnrFileVersion + 1;
  const std::vector<uint8_t> wire;
  const auto encoded = EncodeRnrBuffer(header, wire);

  RnrHeader out;
  std::vector<uint8_t> outWire;
  EXPECT_EQ(ParseRnrBuffer(encoded, out, outWire), RnrIoStatus::kVersionMismatch);
}

TEST(RnrFileTest, SaveLoadDiskRoundTrips) {
  const std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / "round_trip.rnr";

  RnrHeader header;
  header.width = 32;
  header.height = 32;
  header.backend = BackendHint::kTinySkia;
  header.uri = "memory://trivial";
  const std::vector<uint8_t> wire = {0xAA, 0xBB, 0xCC};

  ASSERT_EQ(SaveRnrFile(path, header, wire), RnrIoStatus::kOk);
  RnrHeader loaded;
  std::vector<uint8_t> loadedWire;
  ASSERT_EQ(LoadRnrFile(path, loaded, loadedWire), RnrIoStatus::kOk);
  EXPECT_EQ(loaded.width, 32u);
  EXPECT_EQ(loaded.height, 32u);
  EXPECT_EQ(loaded.uri, "memory://trivial");
  EXPECT_EQ(loadedWire, wire);
}

// -----------------------------------------------------------------------------
// FrameInspector decoding
// -----------------------------------------------------------------------------

TEST(FrameInspectorTest, DecodesKnownSerializedFrame) {
  const auto wire = SerializeSimpleFrame(kSvg, 64, 64);
  const auto result = FrameInspector::Decode(wire);

  ASSERT_TRUE(result.streamValid) << result.error;
  ASSERT_FALSE(result.commands.empty());

  // First command is always the stream header.
  EXPECT_EQ(result.commands.front().opcode, Opcode::kStreamHeader);

  // Somewhere in there we must see a beginFrame and an endFrame, and at
  // least two draw primitives (the SVG has two nested rects — the driver
  // may encode them as drawRect or drawPath depending on how it resolves
  // the shape geometry; either is fine for this test).
  bool sawBegin = false;
  bool sawEnd = false;
  int drawCount = 0;
  for (const auto& cmd : result.commands) {
    if (cmd.opcode == Opcode::kBeginFrame) sawBegin = true;
    if (cmd.opcode == Opcode::kEndFrame) sawEnd = true;
    if (cmd.opcode == Opcode::kDrawRect || cmd.opcode == Opcode::kDrawPath ||
        cmd.opcode == Opcode::kDrawEllipse) {
      ++drawCount;
    }
    EXPECT_GE(cmd.depth, 0);
    EXPECT_FALSE(cmd.summary.empty()) << "opcode "
                                       << static_cast<int>(cmd.opcode)
                                       << " should have a summary";
  }
  EXPECT_TRUE(sawBegin);
  EXPECT_TRUE(sawEnd);
  EXPECT_GE(drawCount, 2);
  EXPECT_EQ(result.finalDepth, 0) << "push/pop must balance for a clean frame";
}

TEST(FrameInspectorTest, DumpProducesTextOutput) {
  const auto wire = SerializeSimpleFrame(kSvg, 32, 32);
  const std::string dump = FrameInspector::Dump(wire);
  EXPECT_FALSE(dump.empty());
  EXPECT_NE(dump.find("beginFrame"), std::string::npos);
  EXPECT_NE(dump.find("endFrame"), std::string::npos);
}

TEST(FrameInspectorTest, DecodeStopsAtTruncation) {
  auto wire = SerializeSimpleFrame(kSvg, 32, 32);
  // Cut off the middle of a message to force a truncation error.
  wire.resize(wire.size() / 2);
  const auto result = FrameInspector::Decode(wire);
  // Valid prefix was decoded but stream is not valid overall.
  EXPECT_FALSE(result.commands.empty());
  EXPECT_FALSE(result.streamValid);
  EXPECT_FALSE(result.error.empty());
}

TEST(FrameInspectorTest, OpcodeNamesAreStable) {
  // Every listed opcode should have a short name.
  const std::array<Opcode, 14> opcodes = {
      Opcode::kStreamHeader, Opcode::kBeginFrame, Opcode::kEndFrame,
      Opcode::kSetTransform, Opcode::kPushTransform, Opcode::kPopTransform,
      Opcode::kPushClip, Opcode::kPopClip, Opcode::kPushIsolatedLayer,
      Opcode::kPopIsolatedLayer, Opcode::kSetPaint, Opcode::kDrawPath,
      Opcode::kDrawRect, Opcode::kDrawEllipse};
  for (Opcode op : opcodes) {
    const auto name = FrameInspector::OpcodeName(op);
    EXPECT_FALSE(name.empty()) << "opcode " << static_cast<int>(op);
    EXPECT_NE(name, "unknown");
  }
}

// -----------------------------------------------------------------------------
// Partial replay — scrub slider simulation
// -----------------------------------------------------------------------------

TEST(FrameInspectorTest, ReplayPrefixFullStreamMatchesDirect) {
  const auto wire = SerializeSimpleFrame(kSvg, 64, 64);

  svg::RendererTinySkia target;
  const auto status = FrameInspector::ReplayPrefix(
      wire, std::numeric_limits<std::size_t>::max(), target);
  ASSERT_EQ(status, ReplayStatus::kOk);

  const auto direct = RenderInProcess(kSvg, 64, 64);
  const auto viaInspector = target.takeSnapshot();
  ASSERT_EQ(direct.dimensions, viaInspector.dimensions);
  EXPECT_EQ(direct.pixels, viaInspector.pixels);
}

TEST(FrameInspectorTest, ReplayPrefixZeroProducesBlankFrame) {
  const auto wire = SerializeSimpleFrame(kSvg, 32, 32);

  svg::RendererTinySkia target;
  const auto status = FrameInspector::ReplayPrefix(wire, 0, target);
  EXPECT_EQ(status, ReplayStatus::kOk);

  const auto snap = target.takeSnapshot();
  // No beginFrame happened, so the backend never allocated a framebuffer.
  // Its dimensions should be zero.
  EXPECT_EQ(snap.dimensions.x, 0);
  EXPECT_EQ(snap.dimensions.y, 0);
}

TEST(FrameInspectorTest, ReplayPrefixPartialIsRenderable) {
  const auto wire = SerializeSimpleFrame(kSvg, 32, 32);
  const auto decoded = FrameInspector::Decode(wire);
  ASSERT_TRUE(decoded.streamValid);

  // Replay the first 3 commands (beginFrame + a push + a setPaint, most
  // likely). The exact sequence depends on the driver, but the target must
  // end up with a valid framebuffer of the right size thanks to the
  // synthesized endFrame.
  svg::RendererTinySkia target;
  const auto status = FrameInspector::ReplayPrefix(wire, 3, target);
  EXPECT_EQ(status, ReplayStatus::kOk);
  const auto snap = target.takeSnapshot();
  EXPECT_GT(snap.dimensions.x, 0);
  EXPECT_GT(snap.dimensions.y, 0);
}

// -----------------------------------------------------------------------------
// End-to-end: record → save → load → replay → pixel-identical
// -----------------------------------------------------------------------------

TEST(RecordReplayEndToEndTest, RoundTripPreservesPixels) {
  const auto wire = SerializeSimpleFrame(kSvg, 100, 100);

  RnrHeader header;
  header.width = 100;
  header.height = 100;
  header.backend = BackendHint::kTinySkia;
  header.uri = "memory://record_replay_test";

  const std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / "round_trip_full.rnr";
  ASSERT_EQ(SaveRnrFile(path, header, wire), RnrIoStatus::kOk);

  RnrHeader loadedHeader;
  std::vector<uint8_t> loadedWire;
  ASSERT_EQ(LoadRnrFile(path, loadedHeader, loadedWire), RnrIoStatus::kOk);
  EXPECT_EQ(loadedWire, wire);

  svg::RendererTinySkia target;
  const auto status = FrameInspector::ReplayPrefix(
      loadedWire, std::numeric_limits<std::size_t>::max(), target);
  ASSERT_EQ(status, ReplayStatus::kOk);

  const auto direct = RenderInProcess(kSvg, 100, 100);
  const auto replayed = target.takeSnapshot();
  ASSERT_EQ(direct.dimensions, replayed.dimensions);
  EXPECT_EQ(direct.pixels, replayed.pixels)
      << "round-tripping through a .rnr file on disk must be lossless";
}

}  // namespace
}  // namespace donner::editor::sandbox
