/// @file
///
/// Milestone S2 unit + integration tests for the editor sandbox wire format.
///
/// Layered coverage:
/// 1. Primitive `Wire.h` round-trips — every `write*` pairs with a `read*`.
/// 2. Type codec round-trips — encode → decode → equality across every
///    supported Donner value type (Vector2d, Transform2d, Box2d, RGBA, Color,
///    StrokeParams, Path, PathShape, PaintParams with Solid/None, ResolvedClip).
/// 3. `SerializingRenderer` → `ReplayingRenderer` end-to-end dispatch against
///    a capturing mock backend, verifying the wire preserves call sequence
///    and arguments.
/// 4. Adversarial input: the reader never crashes on truncated or garbage
///    bytes, regardless of the message stream prefix.
///
/// The pixel-exact "parse SVG → render twice → compare PNG" round-trip is
/// intentionally out of scope for this test file; that lives in the renderer
/// test suite once S3 wires the child process up.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "donner/base/FillRule.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Path.h"
#include "donner/css/Color.h"
#include "donner/css/FontFace.h"
#include "donner/editor/sandbox/ReplayingRenderer.h"
#include "donner/editor/sandbox/SandboxCodecs.h"
#include "donner/editor/sandbox/SerializingRenderer.h"
#include "donner/editor/sandbox/Wire.h"
#include "donner/svg/SVG.h"
#include "donner/svg/core/MixBlendMode.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/RendererTinySkia.h"
#include "donner/svg/renderer/StrokeParams.h"

namespace donner::editor::sandbox {
namespace {

using ::donner::svg::MixBlendMode;
using ::donner::svg::PaintParams;
using ::donner::svg::PaintServer;
using ::donner::svg::PathShape;
using ::donner::svg::RenderViewport;
using ::donner::svg::ResolvedClip;
using ::donner::svg::StrokeLinecap;
using ::donner::svg::StrokeLinejoin;
using ::donner::svg::StrokeParams;

// -----------------------------------------------------------------------------
// WireReader/WireWriter primitives
// -----------------------------------------------------------------------------

TEST(WireTest, RoundTripPrimitives) {
  WireWriter w;
  w.writeU8(0x42);
  w.writeU32(0xDEADBEEFu);
  w.writeU64(0x0123456789ABCDEFull);
  w.writeF64(3.14159265358979);
  w.writeBool(true);
  w.writeBool(false);
  w.writeString("hello sandbox");

  WireReader r(w.data());
  uint8_t b = 0;
  uint32_t i32 = 0;
  uint64_t i64 = 0;
  double f = 0;
  bool bTrue = false;
  bool bFalse = true;
  std::string s;

  ASSERT_TRUE(r.readU8(b));
  ASSERT_TRUE(r.readU32(i32));
  ASSERT_TRUE(r.readU64(i64));
  ASSERT_TRUE(r.readF64(f));
  ASSERT_TRUE(r.readBool(bTrue));
  ASSERT_TRUE(r.readBool(bFalse));
  ASSERT_TRUE(r.readString(s));

  EXPECT_EQ(b, 0x42);
  EXPECT_EQ(i32, 0xDEADBEEFu);
  EXPECT_EQ(i64, 0x0123456789ABCDEFull);
  EXPECT_DOUBLE_EQ(f, 3.14159265358979);
  EXPECT_TRUE(bTrue);
  EXPECT_FALSE(bFalse);
  EXPECT_EQ(s, "hello sandbox");
  EXPECT_EQ(r.remaining(), 0u);
  EXPECT_FALSE(r.failed());
}

TEST(WireTest, TruncatedReadFailsGracefully) {
  WireWriter w;
  w.writeU8(0x01);  // only one byte available
  WireReader r(w.data());
  uint32_t i = 0;
  EXPECT_FALSE(r.readU32(i));
  EXPECT_TRUE(r.failed());
}

TEST(WireTest, OversizedStringRejected) {
  WireWriter w;
  w.writeU32(0xFFFFFFFFu);  // claim 4 GB of string data
  WireReader r(w.data());
  std::string s;
  EXPECT_FALSE(r.readString(s));
  EXPECT_TRUE(r.failed());
}

TEST(WireTest, CountRespectsCap) {
  WireWriter w;
  w.writeU32(kMaxVectorCount + 1);
  WireReader r(w.data());
  uint32_t count = 0;
  EXPECT_FALSE(r.readCount(count, kMaxVectorCount));
  EXPECT_TRUE(r.failed());
}

TEST(WireTest, MessageHeaderLengthRejectedWhenTooLarge) {
  WireWriter w;
  w.writeU32(static_cast<uint32_t>(Opcode::kBeginFrame));
  w.writeU32(kMaxPayloadBytes + 1);  // payload length out of bounds
  WireReader r(w.data());
  Opcode op = Opcode::kInvalid;
  uint32_t len = 0;
  EXPECT_FALSE(r.readMessageHeader(op, len));
  EXPECT_TRUE(r.failed());
}

// -----------------------------------------------------------------------------
// Type codec round-trips
// -----------------------------------------------------------------------------

TEST(CodecTest, Vector2dRoundTrip) {
  WireWriter w;
  EncodeVector2d(w, Vector2d(1.5, -2.25));
  WireReader r(w.data());
  Vector2d v;
  ASSERT_TRUE(DecodeVector2d(r, v));
  EXPECT_DOUBLE_EQ(v.x, 1.5);
  EXPECT_DOUBLE_EQ(v.y, -2.25);
}

TEST(CodecTest, Transform2dRoundTrip) {
  WireWriter w;
  Transform2d t;
  t.data[0] = 1.1;
  t.data[1] = 2.2;
  t.data[2] = 3.3;
  t.data[3] = 4.4;
  t.data[4] = 5.5;
  t.data[5] = 6.6;
  EncodeTransform2d(w, t);

  WireReader r(w.data());
  Transform2d out;
  ASSERT_TRUE(DecodeTransform2d(r, out));
  for (int i = 0; i < 6; ++i) {
    EXPECT_DOUBLE_EQ(out.data[i], t.data[i]) << "index " << i;
  }
}

TEST(CodecTest, Box2dRoundTrip) {
  WireWriter w;
  Box2d b(Vector2d(1, 2), Vector2d(10, 20));
  EncodeBox2d(w, b);
  WireReader r(w.data());
  Box2d out;
  ASSERT_TRUE(DecodeBox2d(r, out));
  EXPECT_DOUBLE_EQ(out.topLeft.x, 1);
  EXPECT_DOUBLE_EQ(out.topLeft.y, 2);
  EXPECT_DOUBLE_EQ(out.bottomRight.x, 10);
  EXPECT_DOUBLE_EQ(out.bottomRight.y, 20);
}

TEST(CodecTest, RgbaRoundTrip) {
  WireWriter w;
  const css::RGBA rgba(0x12, 0x34, 0x56, 0x78);
  EncodeRgba(w, rgba);
  WireReader r(w.data());
  css::RGBA out;
  ASSERT_TRUE(DecodeRgba(r, out));
  EXPECT_EQ(out, rgba);
}

TEST(CodecTest, ColorRoundTrip) {
  WireWriter w;
  const css::Color color(css::RGBA(0xFF, 0x80, 0x00, 0xAA));
  EncodeColor(w, color);
  WireReader r(w.data());
  css::Color out(css::RGBA{});
  ASSERT_TRUE(DecodeColor(r, out));
  ASSERT_TRUE(std::holds_alternative<css::RGBA>(out.value));
  EXPECT_EQ(std::get<css::RGBA>(out.value), css::RGBA(0xFF, 0x80, 0x00, 0xAA));
}

TEST(CodecTest, StrokeParamsRoundTrip) {
  WireWriter w;
  StrokeParams s;
  s.strokeWidth = 2.5;
  s.lineCap = StrokeLinecap::Round;
  s.lineJoin = StrokeLinejoin::Bevel;
  s.miterLimit = 5.0;
  s.dashArray = {2.0, 3.5, 1.0};
  s.dashOffset = 0.25;
  s.pathLength = 100.0;
  EncodeStrokeParams(w, s);

  WireReader r(w.data());
  StrokeParams out;
  ASSERT_TRUE(DecodeStrokeParams(r, out));
  EXPECT_DOUBLE_EQ(out.strokeWidth, 2.5);
  EXPECT_EQ(out.lineCap, StrokeLinecap::Round);
  EXPECT_EQ(out.lineJoin, StrokeLinejoin::Bevel);
  EXPECT_DOUBLE_EQ(out.miterLimit, 5.0);
  EXPECT_EQ(out.dashArray, (std::vector<double>{2.0, 3.5, 1.0}));
  EXPECT_DOUBLE_EQ(out.dashOffset, 0.25);
  EXPECT_DOUBLE_EQ(out.pathLength, 100.0);
}

TEST(CodecTest, PathRoundTripPreservesVerbsAndPoints) {
  PathBuilder pb;
  pb.moveTo(Vector2d(1, 2))
      .lineTo(Vector2d(3, 4))
      .quadTo(Vector2d(5, 6), Vector2d(7, 8))
      .curveTo(Vector2d(9, 10), Vector2d(11, 12), Vector2d(13, 14))
      .closePath();
  Path original = pb.build();

  WireWriter w;
  EncodePath(w, original);
  WireReader r(w.data());
  Path out;
  ASSERT_TRUE(DecodePath(r, out));

  ASSERT_EQ(out.commands().size(), original.commands().size());
  for (std::size_t i = 0; i < out.commands().size(); ++i) {
    EXPECT_EQ(out.commands()[i].verb, original.commands()[i].verb) << "index " << i;
  }
  ASSERT_EQ(out.points().size(), original.points().size());
  for (std::size_t i = 0; i < out.points().size(); ++i) {
    EXPECT_EQ(out.points()[i], original.points()[i]) << "index " << i;
  }
}

TEST(CodecTest, PaintParamsRoundTripSolidFill) {
  PaintParams p;
  p.opacity = 0.75;
  p.fill = PaintServer::Solid(css::Color(css::RGBA(255, 0, 0, 255)));
  p.stroke = PaintServer::None{};
  p.fillOpacity = 0.9;
  p.strokeOpacity = 1.0;
  p.currentColor = css::Color(css::RGBA(128, 128, 128, 255));
  p.viewBox = Box2d(Vector2d(0, 0), Vector2d(100, 100));
  p.strokeParams.strokeWidth = 1.25;

  WireWriter w;
  EncodePaintParams(w, p);
  WireReader r(w.data());
  PaintParams out;
  ASSERT_TRUE(DecodePaintParams(r, out));
  EXPECT_DOUBLE_EQ(out.opacity, 0.75);
  ASSERT_TRUE(std::holds_alternative<PaintServer::Solid>(out.fill));
  EXPECT_EQ(std::get<PaintServer::Solid>(out.fill).color.value,
            css::Color::Type(css::RGBA(255, 0, 0, 255)));
  EXPECT_TRUE(std::holds_alternative<PaintServer::None>(out.stroke));
  EXPECT_DOUBLE_EQ(out.fillOpacity, 0.9);
  EXPECT_DOUBLE_EQ(out.strokeOpacity, 1.0);
  EXPECT_DOUBLE_EQ(out.strokeParams.strokeWidth, 1.25);
}

TEST(CodecTest, ResolvedClipRoundTripRectOnly) {
  ResolvedClip c;
  c.clipRect = Box2d(Vector2d(0, 0), Vector2d(50, 50));
  c.clipPathUnitsTransform = Transform2d();
  // No paths, no mask.

  WireWriter w;
  EncodeResolvedClip(w, c);
  WireReader r(w.data());
  ResolvedClip out;
  ASSERT_TRUE(DecodeResolvedClip(r, out));
  ASSERT_TRUE(out.clipRect.has_value());
  EXPECT_DOUBLE_EQ(out.clipRect->topLeft.x, 0);
  EXPECT_DOUBLE_EQ(out.clipRect->bottomRight.x, 50);
  EXPECT_TRUE(out.clipPaths.empty());
  EXPECT_FALSE(out.mask.has_value());
}

// -----------------------------------------------------------------------------
// SerializingRenderer → ReplayingRenderer integration
// -----------------------------------------------------------------------------

namespace capture {

// Minimal RendererInterface that records the calls it receives. The round-trip
// test drives a SerializingRenderer manually, decodes via ReplayingRenderer,
// and asserts the captured sequence matches what was dispatched.
class CapturingRenderer final : public svg::RendererInterface {
public:
  struct Call {
    std::string kind;
    double x = 0, y = 0, w = 0, h = 0;
    double opacity = 1.0;
    uint8_t r = 0, g = 0, b = 0, a = 0;
  };

  std::vector<Call> calls;
  int w_ = 0;
  int h_ = 0;

  void draw(svg::SVGDocument&) override { calls.push_back({"draw"}); }
  int width() const override { return w_; }
  int height() const override { return h_; }

  void beginFrame(const svg::RenderViewport& vp) override {
    w_ = static_cast<int>(vp.size.x);
    h_ = static_cast<int>(vp.size.y);
    Call c{"beginFrame"};
    c.x = vp.size.x;
    c.y = vp.size.y;
    c.opacity = vp.devicePixelRatio;
    calls.push_back(c);
  }
  void endFrame() override { calls.push_back({"endFrame"}); }

  void setTransform(const Transform2d& t) override {
    Call c{"setTransform"};
    c.x = t.data[4];
    c.y = t.data[5];
    calls.push_back(c);
  }
  void pushTransform(const Transform2d& t) override {
    Call c{"pushTransform"};
    c.x = t.data[4];
    c.y = t.data[5];
    calls.push_back(c);
  }
  void popTransform() override { calls.push_back({"popTransform"}); }

  void pushClip(const svg::ResolvedClip& clip) override {
    Call c{"pushClip"};
    if (clip.clipRect) {
      c.x = clip.clipRect->topLeft.x;
      c.y = clip.clipRect->topLeft.y;
      c.w = clip.clipRect->bottomRight.x;
      c.h = clip.clipRect->bottomRight.y;
    }
    calls.push_back(c);
  }
  void popClip() override { calls.push_back({"popClip"}); }

  void pushIsolatedLayer(double opacity, svg::MixBlendMode) override {
    Call c{"pushIsolatedLayer"};
    c.opacity = opacity;
    calls.push_back(c);
  }
  void popIsolatedLayer() override { calls.push_back({"popIsolatedLayer"}); }

  void pushFilterLayer(const svg::components::FilterGraph&, const std::optional<Box2d>&) override {
  }
  void popFilterLayer() override {}

  void pushMask(const std::optional<Box2d>&) override {}
  void transitionMaskToContent() override {}
  void popMask() override {}

  void beginPatternTile(const Box2d&, const Transform2d&) override {}
  void endPatternTile(bool) override {}

  void setPaint(const svg::PaintParams& paint) override {
    Call c{"setPaint"};
    c.opacity = paint.opacity;
    if (std::holds_alternative<svg::PaintServer::Solid>(paint.fill)) {
      const auto& solid = std::get<svg::PaintServer::Solid>(paint.fill);
      if (std::holds_alternative<css::RGBA>(solid.color.value)) {
        const auto rgba = std::get<css::RGBA>(solid.color.value);
        c.r = rgba.r;
        c.g = rgba.g;
        c.b = rgba.b;
        c.a = rgba.a;
      }
    }
    calls.push_back(c);
  }

  void drawPath(const svg::PathShape& shape, const svg::StrokeParams&) override {
    Call c{"drawPath"};
    c.opacity = static_cast<double>(shape.path.commands().size());
    calls.push_back(c);
  }
  void drawRect(const Box2d& rect, const svg::StrokeParams&) override {
    Call c{"drawRect"};
    c.x = rect.topLeft.x;
    c.y = rect.topLeft.y;
    c.w = rect.bottomRight.x;
    c.h = rect.bottomRight.y;
    calls.push_back(c);
  }
  void drawEllipse(const Box2d& bounds, const svg::StrokeParams&) override {
    Call c{"drawEllipse"};
    c.x = bounds.topLeft.x;
    c.y = bounds.topLeft.y;
    c.w = bounds.bottomRight.x;
    c.h = bounds.bottomRight.y;
    calls.push_back(c);
  }
  void drawImage(const svg::ImageResource&, const svg::ImageParams&) override {}
  void drawText(Registry&, const svg::components::ComputedTextComponent&,
                const svg::TextParams&) override {}

  svg::RendererBitmap takeSnapshot() const override { return {}; }
  std::unique_ptr<svg::RendererInterface> createOffscreenInstance() const override {
    return nullptr;
  }
};

}  // namespace capture

TEST(IntegrationTest, SerializeReplayRoundTripDispatchesCallsInOrder) {
  SerializingRenderer ser;
  // Simulate a minimal driver-driven sequence by calling the supported methods
  // in the same order the driver would.
  ser.beginFrame({Vector2d(100, 80), 1.0});
  ser.pushTransform(Transform2d());
  PaintParams paint;
  paint.opacity = 0.5;
  paint.fill = PaintServer::Solid(css::Color(css::RGBA(1, 2, 3, 4)));
  ser.setPaint(paint);
  ser.drawRect(Box2d(Vector2d(10, 20), Vector2d(30, 40)), StrokeParams{});
  ser.popTransform();
  ser.endFrame();

  ASSERT_FALSE(ser.hasUnsupported());

  capture::CapturingRenderer target;
  ReplayingRenderer replay(target);
  ReplayReport report;
  const auto status = replay.pumpFrame(ser.data(), report);
  EXPECT_EQ(status, ReplayStatus::kOk);
  EXPECT_EQ(report.unsupportedCount, 0u);

  ASSERT_EQ(target.calls.size(), 6u);
  EXPECT_EQ(target.calls[0].kind, "beginFrame");
  EXPECT_DOUBLE_EQ(target.calls[0].x, 100);
  EXPECT_DOUBLE_EQ(target.calls[0].y, 80);
  EXPECT_EQ(target.calls[1].kind, "pushTransform");
  EXPECT_EQ(target.calls[2].kind, "setPaint");
  EXPECT_DOUBLE_EQ(target.calls[2].opacity, 0.5);
  EXPECT_EQ(target.calls[2].r, 1);
  EXPECT_EQ(target.calls[2].g, 2);
  EXPECT_EQ(target.calls[2].b, 3);
  EXPECT_EQ(target.calls[2].a, 4);
  EXPECT_EQ(target.calls[3].kind, "drawRect");
  EXPECT_DOUBLE_EQ(target.calls[3].x, 10);
  EXPECT_DOUBLE_EQ(target.calls[3].y, 20);
  EXPECT_DOUBLE_EQ(target.calls[3].w, 30);
  EXPECT_DOUBLE_EQ(target.calls[3].h, 40);
  EXPECT_EQ(target.calls[4].kind, "popTransform");
  EXPECT_EQ(target.calls[5].kind, "endFrame");
}

TEST(IntegrationTest, AllMethodsNowSupportedNoUnsupported) {
  // Every RendererInterface method now has a real opcode. This test
  // verifies there are zero kUnsupported emissions for a frame that
  // exercises drawText (the last method to get a real opcode).
  SerializingRenderer ser;
  ser.beginFrame({Vector2d(10, 10), 1.0});
  Registry dummyRegistry;
  svg::components::ComputedTextComponent dummyText;
  ser.drawText(dummyRegistry, dummyText, svg::TextParams{});
  ser.endFrame();
  EXPECT_FALSE(ser.hasUnsupported())
      << "drawText should now encode as a real opcode, not kUnsupported";
  EXPECT_EQ(ser.unsupportedCount(), 0u);

  capture::CapturingRenderer target;
  ReplayingRenderer replay(target);
  ReplayReport report;
  const auto status = replay.pumpFrame(ser.data(), report);
  EXPECT_EQ(status, ReplayStatus::kOk)
      << "replay should complete cleanly with zero unsupported";
  EXPECT_EQ(report.unsupportedCount, 0u);
}

// -----------------------------------------------------------------------------
// Adversarial inputs — the deserializer must never crash
// -----------------------------------------------------------------------------

TEST(AdversarialTest, RandomBytesDoNotCrashReplay) {
  // 256 bytes of "random-ish" input across a few characteristic patterns.
  const std::vector<std::vector<uint8_t>> corpora = {
      {},                                                      // empty
      {0x00},                                                  // too short for header
      {0xDE, 0xAD, 0xBE, 0xEF},                                // wrong magic
      {0x44, 0x52, 0x4E, 0x52},                                // correct magic alone
      std::vector<uint8_t>(32, 0xFF),                          // all ones
      std::vector<uint8_t>(256, 0x00),                         // all zeros
  };

  capture::CapturingRenderer target;
  ReplayingRenderer replay(target);
  for (const auto& bytes : corpora) {
    ReplayReport report;
    const auto status = replay.pumpFrame(bytes, report);
    (void)status;  // any status is fine — all that matters is "no crash".
  }
}

// -----------------------------------------------------------------------------
// Pixel-exact RendererTinySkia round-trip — the canonical S2 lossless check.
// -----------------------------------------------------------------------------

namespace {

svg::SVGDocument ParseOrDie(std::string_view svg) {
  ParseWarningSink warnings;
  auto result = svg::parser::SVGParser::ParseSVG(svg, warnings);
  EXPECT_FALSE(result.hasError()) << result.error();
  return std::move(result.result());
}

svg::RendererBitmap RenderDirect(std::string_view svg, int width, int height) {
  auto document = ParseOrDie(svg);
  document.setCanvasSize(width, height);
  svg::RendererTinySkia renderer;
  renderer.draw(document);
  return renderer.takeSnapshot();
}

svg::RendererBitmap RenderViaWire(std::string_view svg, int width, int height,
                                  bool& outHasUnsupported) {
  auto serDocument = ParseOrDie(svg);
  serDocument.setCanvasSize(width, height);

  SerializingRenderer ser;
  ser.draw(serDocument);
  outHasUnsupported = ser.hasUnsupported();

  svg::RendererTinySkia replayBackend;
  ReplayingRenderer replay(replayBackend);
  ReplayReport report;
  const auto status = replay.pumpFrame(ser.data(), report);
  EXPECT_TRUE(status == ReplayStatus::kOk || status == ReplayStatus::kEncounteredUnsupported)
      << "replay status = " << static_cast<int>(status);
  return replayBackend.takeSnapshot();
}

}  // namespace

TEST(PixelRoundTripTest, SolidRectMatchesDirectRender) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
         <rect width="32" height="32" fill="red"/>
       </svg>)";

  const auto direct = RenderDirect(kSvg, 32, 32);
  bool hasUnsupported = false;
  const auto viaWire = RenderViaWire(kSvg, 32, 32, hasUnsupported);

  ASSERT_EQ(direct.dimensions, viaWire.dimensions);
  ASSERT_EQ(direct.pixels.size(), viaWire.pixels.size());
  EXPECT_FALSE(hasUnsupported);
  EXPECT_EQ(direct.pixels, viaWire.pixels)
      << "serializing/replaying through the wire format must be pixel-identical";
}

TEST(PixelRoundTripTest, PathFillMatchesDirectRender) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
         <path d="M8 8 L56 8 L32 56 Z" fill="blue"/>
       </svg>)";

  const auto direct = RenderDirect(kSvg, 64, 64);
  bool hasUnsupported = false;
  const auto viaWire = RenderViaWire(kSvg, 64, 64, hasUnsupported);

  ASSERT_EQ(direct.dimensions, viaWire.dimensions);
  ASSERT_EQ(direct.pixels.size(), viaWire.pixels.size());
  EXPECT_FALSE(hasUnsupported);
  EXPECT_EQ(direct.pixels, viaWire.pixels);
}

TEST(PixelRoundTripTest, LinearGradientMatchesDirectRender) {
  // WIRE.5 exercise: a linear gradient filled rect. The serializer
  // flattens the PaintResolvedReference into a WireGradient; the replayer
  // materializes a fresh entity in its private registry and hands the
  // backend a real PaintResolvedReference that points at it.
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="80" height="80">
         <defs>
           <linearGradient id="g" x1="0" y1="0" x2="80" y2="0"
                           gradientUnits="userSpaceOnUse">
             <stop offset="0" stop-color="#ff0000"/>
             <stop offset="1" stop-color="#0000ff"/>
           </linearGradient>
         </defs>
         <rect width="80" height="80" fill="url(#g)"/>
       </svg>)svg";

  const auto direct = RenderDirect(kSvg, 80, 80);
  bool hasUnsupported = false;
  const auto viaWire = RenderViaWire(kSvg, 80, 80, hasUnsupported);

  ASSERT_EQ(direct.dimensions, viaWire.dimensions);
  ASSERT_EQ(direct.pixels.size(), viaWire.pixels.size());
  EXPECT_FALSE(hasUnsupported)
      << "linear gradients should no longer trigger kUnsupported";
  EXPECT_EQ(direct.pixels, viaWire.pixels)
      << "linear gradient rendering must be byte-identical across the wire";
}

TEST(PixelRoundTripTest, RadialGradientMatchesDirectRender) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="80" height="80">
         <defs>
           <radialGradient id="g" cx="40" cy="40" r="40"
                           gradientUnits="userSpaceOnUse">
             <stop offset="0" stop-color="#ffff00"/>
             <stop offset="1" stop-color="#ff00ff"/>
           </radialGradient>
         </defs>
         <circle cx="40" cy="40" r="40" fill="url(#g)"/>
       </svg>)svg";

  const auto direct = RenderDirect(kSvg, 80, 80);
  bool hasUnsupported = false;
  const auto viaWire = RenderViaWire(kSvg, 80, 80, hasUnsupported);

  ASSERT_EQ(direct.dimensions, viaWire.dimensions);
  EXPECT_FALSE(hasUnsupported);
  EXPECT_EQ(direct.pixels, viaWire.pixels)
      << "radial gradient rendering must be byte-identical across the wire";
}

TEST(PixelRoundTripTest, MaskedContentMatchesDirectRender) {
  // Exercise kPushMask/kTransitionMaskToContent/kPopMask — a solid rect
  // under a circular mask. If the wire drops any of these, the result
  // comes out as an unmasked full rect.
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="80" height="80">
         <defs>
           <mask id="m">
             <circle cx="40" cy="40" r="25" fill="white"/>
           </mask>
         </defs>
         <rect width="80" height="80" fill="red" mask="url(#m)"/>
       </svg>)svg";

  const auto direct = RenderDirect(kSvg, 80, 80);
  bool hasUnsupported = false;
  const auto viaWire = RenderViaWire(kSvg, 80, 80, hasUnsupported);
  EXPECT_FALSE(hasUnsupported)
      << "mask opcodes should no longer emit kUnsupported";
  ASSERT_EQ(direct.dimensions, viaWire.dimensions);
  EXPECT_EQ(direct.pixels, viaWire.pixels);
}

TEST(PixelRoundTripTest, PatternTileMatchesDirectRender) {
  // Pattern paint servers use a backend-side channel: beginPatternTile →
  // sub-stream → endPatternTile stores the recorded pixmap in
  // `patternFillPaint_`, and the next makeFillPaint call picks it up
  // regardless of what the wire says in setPaint (which still encodes the
  // PaintResolvedReference as kPaintTagStub → None). The critical
  // correctness guarantee is that the recorded tile pixmap and the backend's
  // state management survive the serialize/replay roundtrip.
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="80" height="80">
         <defs>
           <pattern id="p" x="0" y="0" width="16" height="16"
                    patternUnits="userSpaceOnUse">
             <rect width="8" height="8" fill="blue"/>
             <rect x="8" y="8" width="8" height="8" fill="blue"/>
           </pattern>
         </defs>
         <rect width="80" height="80" fill="url(#p)"/>
       </svg>)svg";

  const auto direct = RenderDirect(kSvg, 80, 80);
  bool hasUnsupported = false;
  const auto viaWire = RenderViaWire(kSvg, 80, 80, hasUnsupported);
  EXPECT_FALSE(hasUnsupported)
      << "pattern opcodes should not emit kUnsupported";
  ASSERT_EQ(direct.dimensions, viaWire.dimensions);

  // Count differing pixels and diagnose.
  int diffPixels = 0;
  int nonBlankDirect = 0;
  int nonBlankWire = 0;
  for (std::size_t i = 0; i + 3 < direct.pixels.size(); i += 4) {
    if (direct.pixels[i + 3] != 0) ++nonBlankDirect;
    if (viaWire.pixels[i + 3] != 0) ++nonBlankWire;
    if (direct.pixels[i] != viaWire.pixels[i] ||
        direct.pixels[i + 1] != viaWire.pixels[i + 1] ||
        direct.pixels[i + 2] != viaWire.pixels[i + 2] ||
        direct.pixels[i + 3] != viaWire.pixels[i + 3]) {
      ++diffPixels;
    }
  }
  const int totalPixels = direct.dimensions.x * direct.dimensions.y;
  // Also verify deterministic serialization: two separate serialize passes
  // on independently-parsed documents must produce identical wire bytes.
  auto wireDoc1 = ParseOrDie(kSvg);
  wireDoc1.setCanvasSize(80, 80);
  SerializingRenderer ser1;
  ser1.draw(wireDoc1);
  auto wireDoc2 = ParseOrDie(kSvg);
  wireDoc2.setCanvasSize(80, 80);
  SerializingRenderer ser2;
  ser2.draw(wireDoc2);
  const bool wiresDeterministic = (ser1.data().size() == ser2.data().size()) &&
      std::equal(ser1.data().begin(), ser1.data().end(), ser2.data().begin());
  EXPECT_TRUE(wiresDeterministic)
      << "two serialize passes produced different wire bytes (sizes: "
      << ser1.data().size() << " vs " << ser2.data().size() << ")";
  EXPECT_EQ(direct.pixels, viaWire.pixels)
      << "pattern: " << diffPixels << " / " << totalPixels << " pixels differ. "
      << "direct non-blank=" << nonBlankDirect
      << " wire non-blank=" << nonBlankWire;
}

TEST(PixelRoundTripTest, TransformedShapesMatchDirectRender) {
  // Use a custom raw-string delimiter so the `translate(10 10)` sequence
  // doesn't prematurely terminate the string literal.
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <g transform="translate(10 10)">
           <rect width="30" height="30" fill="green"/>
           <circle cx="60" cy="60" r="20" fill="orange"/>
         </g>
       </svg>)svg";

  const auto direct = RenderDirect(kSvg, 100, 100);
  bool hasUnsupported = false;
  const auto viaWire = RenderViaWire(kSvg, 100, 100, hasUnsupported);

  ASSERT_EQ(direct.dimensions, viaWire.dimensions);
  ASSERT_EQ(direct.pixels.size(), viaWire.pixels.size());
  EXPECT_FALSE(hasUnsupported);
  EXPECT_EQ(direct.pixels, viaWire.pixels);
}

TEST(PixelRoundTripTest, GaussianBlurFilterMatchesDirectRender) {
  // Exercise the full FilterGraph encoding: a single feGaussianBlur
  // primitive applied to a solid red rectangle. The wire must carry the
  // node count, the primitive tag + stdDeviation fields, the input list,
  // and the graph metadata so the replay backend produces the same blur.
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="80" height="80">
         <defs>
           <filter id="f">
             <feGaussianBlur in="SourceGraphic" stdDeviation="3"/>
           </filter>
         </defs>
         <rect width="80" height="80" fill="red" filter="url(#f)"/>
       </svg>)svg";

  const auto direct = RenderDirect(kSvg, 80, 80);
  bool hasUnsupported = false;
  const auto viaWire = RenderViaWire(kSvg, 80, 80, hasUnsupported);
  EXPECT_FALSE(hasUnsupported)
      << "filter opcodes should not emit kUnsupported";
  ASSERT_EQ(direct.dimensions, viaWire.dimensions);
  EXPECT_EQ(direct.pixels, viaWire.pixels)
      << "feGaussianBlur rendering must be byte-identical across the wire";
}

TEST(PixelRoundTripTest, ColorMatrixFilterMatchesDirectRender) {
  // Exercise feColorMatrix (saturate type). A green rect desaturated to
  // grayscale verifies that the matrix type + values vector survive the
  // wire round-trip.
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
         <defs>
           <filter id="f">
             <feColorMatrix type="saturate" values="0"/>
           </filter>
         </defs>
         <rect width="64" height="64" fill="green" filter="url(#f)"/>
       </svg>)svg";

  const auto direct = RenderDirect(kSvg, 64, 64);
  bool hasUnsupported = false;
  const auto viaWire = RenderViaWire(kSvg, 64, 64, hasUnsupported);
  EXPECT_FALSE(hasUnsupported)
      << "feColorMatrix should not emit kUnsupported";
  ASSERT_EQ(direct.dimensions, viaWire.dimensions);
  EXPECT_EQ(direct.pixels, viaWire.pixels)
      << "feColorMatrix rendering must be byte-identical across the wire";
}

TEST(AdversarialTest, ClaimedLengthBeyondBufferFails) {
  // Handcraft a stream that says "here's 1 GB of payload" but gives only 4 bytes.
  WireWriter w;
  w.writeU32(static_cast<uint32_t>(Opcode::kStreamHeader));
  w.writeU32(8);
  w.writeU32(kWireMagic);
  w.writeU32(kWireVersion);

  w.writeU32(static_cast<uint32_t>(Opcode::kBeginFrame));
  w.writeU32(1u << 30);  // 1 GiB
  // ... no payload follows.

  capture::CapturingRenderer target;
  ReplayingRenderer replay(target);
  ReplayReport report;
  EXPECT_EQ(replay.pumpFrame(w.data(), report), ReplayStatus::kMalformed);
}

// -----------------------------------------------------------------------------
// FontFace round-trip
// -----------------------------------------------------------------------------

TEST(CodecTest, FontFaceRoundTrip) {
  css::FontFace face;
  face.familyName = RcString("TestFont");
  face.fontWeight = 700;
  face.fontStyle = 1;
  face.fontStretch = 3;

  // Source 0: Kind::Data with dummy TTF bytes.
  {
    css::FontFaceSource src;
    src.kind = css::FontFaceSource::Kind::Data;
    auto data = std::make_shared<std::vector<uint8_t>>(
        std::vector<uint8_t>{0x00, 0x01, 0x00, 0x00, 0xDE, 0xAD, 0xBE, 0xEF});
    src.payload = std::move(data);
    src.formatHint = RcString("opentype");
    src.techHints = {RcString("variations"), RcString("color-COLRv1")};
    face.sources.push_back(std::move(src));
  }

  // Source 1: Kind::Local with a family name.
  {
    css::FontFaceSource src;
    src.kind = css::FontFaceSource::Kind::Local;
    src.payload = RcString("Arial");
    src.formatHint = RcString("");
    face.sources.push_back(std::move(src));
  }

  // Source 2: Kind::Url with a URL string.
  {
    css::FontFaceSource src;
    src.kind = css::FontFaceSource::Kind::Url;
    src.payload = RcString("https://example.com/font.woff2");
    src.formatHint = RcString("woff2");
    face.sources.push_back(std::move(src));
  }

  WireWriter w;
  EncodeFontFace(w, face);
  WireReader r(w.data());
  css::FontFace decoded;
  ASSERT_TRUE(DecodeFontFace(r, decoded));

  EXPECT_EQ(std::string_view(decoded.familyName), "TestFont");
  EXPECT_EQ(decoded.fontWeight, 700);
  EXPECT_EQ(decoded.fontStyle, 1);
  EXPECT_EQ(decoded.fontStretch, 3);
  ASSERT_EQ(decoded.sources.size(), 3u);

  // Verify Data source round-trips.
  {
    const auto& s = decoded.sources[0];
    EXPECT_EQ(s.kind, css::FontFaceSource::Kind::Data);
    const auto* dataPtr =
        std::get_if<std::shared_ptr<const std::vector<uint8_t>>>(&s.payload);
    ASSERT_NE(dataPtr, nullptr);
    ASSERT_NE(*dataPtr, nullptr);
    EXPECT_EQ((*dataPtr)->size(), 8u);
    EXPECT_EQ((**dataPtr)[4], 0xDE);
    EXPECT_EQ((**dataPtr)[7], 0xEF);
    EXPECT_EQ(std::string_view(s.formatHint), "opentype");
    ASSERT_EQ(s.techHints.size(), 2u);
    EXPECT_EQ(std::string_view(s.techHints[0]), "variations");
    EXPECT_EQ(std::string_view(s.techHints[1]), "color-COLRv1");
  }

  // Verify Local source round-trips.
  {
    const auto& s = decoded.sources[1];
    EXPECT_EQ(s.kind, css::FontFaceSource::Kind::Local);
    const auto* strPtr = std::get_if<RcString>(&s.payload);
    ASSERT_NE(strPtr, nullptr);
    EXPECT_EQ(std::string_view(*strPtr), "Arial");
  }

  // Verify Url source round-trips.
  {
    const auto& s = decoded.sources[2];
    EXPECT_EQ(s.kind, css::FontFaceSource::Kind::Url);
    const auto* strPtr = std::get_if<RcString>(&s.payload);
    ASSERT_NE(strPtr, nullptr);
    EXPECT_EQ(std::string_view(*strPtr), "https://example.com/font.woff2");
    EXPECT_EQ(std::string_view(s.formatHint), "woff2");
  }
}

TEST(CodecTest, TextParamsWithFontFacesRoundTrip) {
  // Build a TextParams with a non-empty fontFaces vector.
  css::FontFace face;
  face.familyName = RcString("CustomFont");
  face.fontWeight = 400;
  face.fontStyle = 0;
  face.fontStretch = 5;

  css::FontFaceSource src;
  src.kind = css::FontFaceSource::Kind::Data;
  auto data = std::make_shared<std::vector<uint8_t>>(
      std::vector<uint8_t>{0xCA, 0xFE, 0xBA, 0xBE, 0x01, 0x02, 0x03});
  src.payload = std::move(data);
  src.formatHint = RcString("truetype");
  face.sources.push_back(std::move(src));

  std::vector<css::FontFace> faces = {face};

  svg::TextParams params;
  params.opacity = 0.8;
  params.fontFamilies.push_back(RcString("CustomFont"));
  params.fontFaces = faces;

  WireWriter w;
  EncodeTextParams(w, params);
  WireReader r(w.data());

  svg::TextParams decoded;
  std::vector<css::FontFace> decodedFaces;
  ASSERT_TRUE(DecodeTextParams(r, decoded, &decodedFaces));

  // Verify the font faces survived the round-trip.
  ASSERT_EQ(decodedFaces.size(), 1u);
  EXPECT_EQ(std::string_view(decodedFaces[0].familyName), "CustomFont");
  ASSERT_EQ(decodedFaces[0].sources.size(), 1u);
  const auto& ds = decodedFaces[0].sources[0];
  EXPECT_EQ(ds.kind, css::FontFaceSource::Kind::Data);
  const auto* dp =
      std::get_if<std::shared_ptr<const std::vector<uint8_t>>>(&ds.payload);
  ASSERT_NE(dp, nullptr);
  ASSERT_NE(*dp, nullptr);
  EXPECT_EQ((*dp)->size(), 7u);
  EXPECT_EQ((**dp)[0], 0xCA);
  EXPECT_EQ((**dp)[3], 0xBE);
  EXPECT_EQ(std::string_view(ds.formatHint), "truetype");

  // Verify the span was set.
  EXPECT_EQ(decoded.fontFaces.size(), 1u);
  EXPECT_DOUBLE_EQ(decoded.opacity, 0.8);
}

}  // namespace
}  // namespace donner::editor::sandbox
