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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
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
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/core/MixBlendMode.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/ResolvedGradient.h"
#include "donner/svg/renderer/StrokeParams.h"

namespace donner::editor::sandbox {
namespace {

using ::testing::DoubleEq;
using ::testing::ElementsAre;

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

  void pushFilterLayer(const svg::components::FilterGraph&, const std::optional<Box2d>&) override {}
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
  EXPECT_EQ(status, ReplayStatus::kOk) << "replay should complete cleanly with zero unsupported";
  EXPECT_EQ(report.unsupportedCount, 0u);
}

// -----------------------------------------------------------------------------
// Adversarial inputs — the deserializer must never crash
// -----------------------------------------------------------------------------

TEST(AdversarialTest, RandomBytesDoNotCrashReplay) {
  // 256 bytes of "random-ish" input across a few characteristic patterns.
  const std::vector<std::vector<uint8_t>> corpora = {
      {},                               // empty
      {0x00},                           // too short for header
      {0xDE, 0xAD, 0xBE, 0xEF},         // wrong magic
      {0x44, 0x52, 0x4E, 0x52},         // correct magic alone
      std::vector<uint8_t>(32, 0xFF),   // all ones
      std::vector<uint8_t>(256, 0x00),  // all zeros
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
// Pixel-exact round-trip — the canonical S2 lossless check.
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
  svg::Renderer renderer;
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

  svg::Renderer replayBackend;
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
  EXPECT_FALSE(hasUnsupported) << "linear gradients should no longer trigger kUnsupported";
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
  EXPECT_FALSE(hasUnsupported) << "mask opcodes should no longer emit kUnsupported";
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
  EXPECT_FALSE(hasUnsupported) << "pattern opcodes should not emit kUnsupported";
  ASSERT_EQ(direct.dimensions, viaWire.dimensions);

  // Count differing pixels and diagnose.
  int diffPixels = 0;
  int nonBlankDirect = 0;
  int nonBlankWire = 0;
  for (std::size_t i = 0; i + 3 < direct.pixels.size(); i += 4) {
    if (direct.pixels[i + 3] != 0) ++nonBlankDirect;
    if (viaWire.pixels[i + 3] != 0) ++nonBlankWire;
    if (direct.pixels[i] != viaWire.pixels[i] || direct.pixels[i + 1] != viaWire.pixels[i + 1] ||
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
  const bool wiresDeterministic =
      (ser1.data().size() == ser2.data().size()) &&
      std::equal(ser1.data().begin(), ser1.data().end(), ser2.data().begin());
  EXPECT_TRUE(wiresDeterministic) << "two serialize passes produced different wire bytes (sizes: "
                                  << ser1.data().size() << " vs " << ser2.data().size() << ")";
  EXPECT_EQ(direct.pixels, viaWire.pixels)
      << "pattern: " << diffPixels << " / " << totalPixels << " pixels differ. "
      << "direct non-blank=" << nonBlankDirect << " wire non-blank=" << nonBlankWire;
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
  EXPECT_FALSE(hasUnsupported) << "filter opcodes should not emit kUnsupported";
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
  EXPECT_FALSE(hasUnsupported) << "feColorMatrix should not emit kUnsupported";
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
    const auto* dataPtr = std::get_if<std::shared_ptr<const std::vector<uint8_t>>>(&s.payload);
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
  const auto* dp = std::get_if<std::shared_ptr<const std::vector<uint8_t>>>(&ds.payload);
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

// =============================================================================
// Coverage expansion: a round-trip for every Encode/Decode pair, plus
// truncated-buffer fail-safe checks for each decoder. The wire boundary is a
// trust boundary (data arrives from a sandboxed child), so a decoder fed a
// short buffer must return false rather than over-read.
// =============================================================================

namespace {

using ::donner::svg::GradientSpreadMethod;
using ::donner::svg::GradientStop;
using ::donner::svg::GradientUnits;
using ::donner::svg::ImageParams;
using ::donner::svg::ImageResource;

// Named matcher: a Lengthd whose value and unit both match. Prints both sides
// of the mismatch so a failure localizes the offending field without a rerun.
MATCHER_P2(LengthdEq, expectedValue, expectedUnit,
           "Lengthd{value=" + ::testing::PrintToString(expectedValue) +
               ", unit=" + ::testing::PrintToString(static_cast<int>(expectedUnit)) + "}") {
  if (arg.value != expectedValue) {
    *result_listener << "value is " << arg.value << " (expected " << expectedValue << ")";
    return false;
  }
  if (arg.unit != expectedUnit) {
    *result_listener << "unit is " << static_cast<int>(arg.unit) << " (expected "
                     << static_cast<int>(expectedUnit) << ")";
    return false;
  }
  return true;
}

// Named matcher: a Box2d whose four corners match. Reports all four channels.
MATCHER_P4(Box2dCorners, tlx, tly, brx, bry,
           "Box2d{(" + ::testing::PrintToString(tlx) + "," + ::testing::PrintToString(tly) + ")-(" +
               ::testing::PrintToString(brx) + "," + ::testing::PrintToString(bry) + ")}") {
  *result_listener << "actual {(" << arg.topLeft.x << "," << arg.topLeft.y << ")-("
                   << arg.bottomRight.x << "," << arg.bottomRight.y << ")}";
  return arg.topLeft.x == tlx && arg.topLeft.y == tly && arg.bottomRight.x == brx &&
         arg.bottomRight.y == bry;
}

}  // namespace

// ---------------------------------------------------------------------------
// Vector2i
// ---------------------------------------------------------------------------

TEST(CodecTest, Vector2iRoundTrip) {
  WireWriter w;
  EncodeVector2i(w, Vector2i(-2147483648, 2147483647));
  WireReader r(w.data());
  Vector2i out;
  ASSERT_TRUE(DecodeVector2i(r, out));
  EXPECT_EQ(out.x, -2147483648);
  EXPECT_EQ(out.y, 2147483647);
  EXPECT_EQ(r.remaining(), 0u);
}

TEST(CodecTest, Vector2iTruncatedFails) {
  WireWriter w;
  w.writeI32(5);  // x present, y missing.
  WireReader r(w.data());
  Vector2i out;
  EXPECT_FALSE(DecodeVector2i(r, out));
}

// ---------------------------------------------------------------------------
// Color: HSLA flattening and CurrentColor fallback
// ---------------------------------------------------------------------------

TEST(CodecTest, ColorHslaFlattenedToRgba) {
  // HSLA is flattened to RGBA on the wire. Round-tripping a fully-saturated red
  // HSLA must come back as RGBA red.
  WireWriter w;
  EncodeColor(w, css::Color(css::HSLA(0, 1.0f, 0.5f, 0xFF)));
  WireReader r(w.data());
  css::Color out(css::RGBA{});
  ASSERT_TRUE(DecodeColor(r, out));
  ASSERT_TRUE(std::holds_alternative<css::RGBA>(out.value));
  EXPECT_EQ(std::get<css::RGBA>(out.value), css::RGBA(255, 0, 0, 255));
}

TEST(CodecTest, ColorCurrentColorEncodesAsDefaultRgba) {
  // CurrentColor is not resolvable at encode time, so the encoder writes a
  // default-constructed css::RGBA() (opaque white) under the RGBA tag.
  WireWriter w;
  EncodeColor(w, css::Color(css::Color::CurrentColor()));
  WireReader r(w.data());
  css::Color out(css::RGBA(1, 2, 3, 4));
  ASSERT_TRUE(DecodeColor(r, out));
  ASSERT_TRUE(std::holds_alternative<css::RGBA>(out.value));
  EXPECT_EQ(std::get<css::RGBA>(out.value), css::RGBA());
}

TEST(CodecTest, ColorRejectsUnknownTag) {
  WireWriter w;
  w.writeU8(99);  // not the RGBA tag (1).
  WireReader r(w.data());
  css::Color out(css::RGBA{});
  EXPECT_FALSE(DecodeColor(r, out));
  EXPECT_TRUE(r.failed());
}

TEST(CodecTest, ColorTruncatedFails) {
  WireWriter w;  // empty: not even the tag byte.
  WireReader r(w.data());
  css::Color out(css::RGBA{});
  EXPECT_FALSE(DecodeColor(r, out));
}

TEST(CodecTest, RgbaTruncatedFails) {
  WireWriter w;
  w.writeU8(0x10);
  w.writeU8(0x20);  // only 2 of 4 channels.
  WireReader r(w.data());
  css::RGBA out;
  EXPECT_FALSE(DecodeRgba(r, out));
}

// ---------------------------------------------------------------------------
// Enums: round-trip every value + out-of-range rejection
// ---------------------------------------------------------------------------

TEST(CodecTest, FillRuleRoundTripAllValues) {
  for (FillRule v : {FillRule::NonZero, FillRule::EvenOdd}) {
    WireWriter w;
    EncodeFillRule(w, v);
    WireReader r(w.data());
    FillRule out = FillRule::NonZero;
    ASSERT_TRUE(DecodeFillRule(r, out));
    EXPECT_EQ(out, v);
  }
}

TEST(CodecTest, FillRuleRejectsOutOfRange) {
  WireWriter w;
  w.writeU8(0xFF);
  WireReader r(w.data());
  FillRule out = FillRule::NonZero;
  EXPECT_FALSE(DecodeFillRule(r, out));
  EXPECT_TRUE(r.failed());
}

TEST(CodecTest, FillRuleTruncatedFails) {
  WireWriter w;
  WireReader r(w.data());
  FillRule out = FillRule::NonZero;
  EXPECT_FALSE(DecodeFillRule(r, out));
}

TEST(CodecTest, MixBlendModeRoundTrip) {
  for (MixBlendMode v : {MixBlendMode::Normal, MixBlendMode::Multiply, MixBlendMode::Luminosity}) {
    WireWriter w;
    EncodeMixBlendMode(w, v);
    WireReader r(w.data());
    MixBlendMode out = MixBlendMode::Normal;
    ASSERT_TRUE(DecodeMixBlendMode(r, out));
    EXPECT_EQ(out, v);
  }
}

TEST(CodecTest, MixBlendModeRejectsOutOfRange) {
  WireWriter w;
  w.writeU8(0xFF);
  WireReader r(w.data());
  MixBlendMode out = MixBlendMode::Normal;
  EXPECT_FALSE(DecodeMixBlendMode(r, out));
  EXPECT_TRUE(r.failed());
}

TEST(CodecTest, StrokeLinecapRoundTrip) {
  for (StrokeLinecap v : {StrokeLinecap::Butt, StrokeLinecap::Round, StrokeLinecap::Square}) {
    WireWriter w;
    EncodeStrokeLinecap(w, v);
    WireReader r(w.data());
    StrokeLinecap out = StrokeLinecap::Butt;
    ASSERT_TRUE(DecodeStrokeLinecap(r, out));
    EXPECT_EQ(out, v);
  }
}

TEST(CodecTest, StrokeLinecapRejectsOutOfRange) {
  WireWriter w;
  w.writeU8(0xFF);
  WireReader r(w.data());
  StrokeLinecap out = StrokeLinecap::Butt;
  EXPECT_FALSE(DecodeStrokeLinecap(r, out));
  EXPECT_TRUE(r.failed());
}

TEST(CodecTest, StrokeLinejoinRoundTrip) {
  for (StrokeLinejoin v : {StrokeLinejoin::Miter, StrokeLinejoin::Round, StrokeLinejoin::Bevel,
                           StrokeLinejoin::Arcs}) {
    WireWriter w;
    EncodeStrokeLinejoin(w, v);
    WireReader r(w.data());
    StrokeLinejoin out = StrokeLinejoin::Miter;
    ASSERT_TRUE(DecodeStrokeLinejoin(r, out));
    EXPECT_EQ(out, v);
  }
}

TEST(CodecTest, StrokeLinejoinRejectsOutOfRange) {
  WireWriter w;
  w.writeU8(0xFF);
  WireReader r(w.data());
  StrokeLinejoin out = StrokeLinejoin::Miter;
  EXPECT_FALSE(DecodeStrokeLinejoin(r, out));
  EXPECT_TRUE(r.failed());
}

// ---------------------------------------------------------------------------
// Lengthd
// ---------------------------------------------------------------------------

TEST(CodecTest, LengthdRoundTrip) {
  WireWriter w;
  EncodeLengthd(w, Lengthd(42.5, LengthUnit::Px));
  EncodeLengthd(w, Lengthd(-3.0, LengthUnit::Vmax));
  WireReader r(w.data());
  Lengthd a, b;
  ASSERT_TRUE(DecodeLengthd(r, a));
  ASSERT_TRUE(DecodeLengthd(r, b));
  EXPECT_THAT(a, LengthdEq(42.5, LengthUnit::Px));
  EXPECT_THAT(b, LengthdEq(-3.0, LengthUnit::Vmax));
}

TEST(CodecTest, LengthdRejectsOutOfRangeUnit) {
  WireWriter w;
  w.writeF64(1.0);
  w.writeU8(0xFF);  // invalid unit.
  WireReader r(w.data());
  Lengthd out;
  EXPECT_FALSE(DecodeLengthd(r, out));
  EXPECT_TRUE(r.failed());
}

TEST(CodecTest, LengthdTruncatedFails) {
  WireWriter w;
  w.writeF64(1.0);  // value present, unit byte missing.
  WireReader r(w.data());
  Lengthd out;
  EXPECT_FALSE(DecodeLengthd(r, out));
}

// ---------------------------------------------------------------------------
// PathShape
// ---------------------------------------------------------------------------

TEST(CodecTest, PathShapeRoundTrip) {
  PathShape shape;
  PathBuilder pb;
  pb.moveTo(Vector2d(0, 0)).lineTo(Vector2d(10, 0)).lineTo(Vector2d(10, 10)).closePath();
  shape.path = pb.build();
  shape.fillRule = FillRule::EvenOdd;
  shape.parentFromEntity = Transform2d::Translate(Vector2d(3, 7));
  shape.layer = 4;

  WireWriter w;
  EncodePathShape(w, shape);
  WireReader r(w.data());
  PathShape out;
  ASSERT_TRUE(DecodePathShape(r, out));
  EXPECT_EQ(out.fillRule, FillRule::EvenOdd);
  EXPECT_EQ(out.layer, 4);
  EXPECT_EQ(out.path.commands().size(), shape.path.commands().size());
  EXPECT_DOUBLE_EQ(out.parentFromEntity.data[4], 3);
  EXPECT_DOUBLE_EQ(out.parentFromEntity.data[5], 7);
}

TEST(CodecTest, PathShapeTruncatedFails) {
  WireWriter w;  // empty buffer can't even read the path command count.
  WireReader r(w.data());
  PathShape out;
  EXPECT_FALSE(DecodePathShape(r, out));
}

TEST(CodecTest, PathRejectsMismatchedPointCount) {
  // One MoveTo verb (expects 1 point) but a declared point count of 0.
  WireWriter w;
  w.writeU32(1);  // command count
  w.writeU8(static_cast<uint8_t>(Path::Verb::MoveTo));
  w.writeU32(0);  // point count — should be 1
  WireReader r(w.data());
  Path out;
  EXPECT_FALSE(DecodePath(r, out));
  EXPECT_TRUE(r.failed());
}

TEST(CodecTest, PathRejectsInvalidVerb) {
  WireWriter w;
  w.writeU32(1);
  w.writeU8(0xFF);  // not a valid verb.
  WireReader r(w.data());
  Path out;
  EXPECT_FALSE(DecodePath(r, out));
  EXPECT_TRUE(r.failed());
}

// ---------------------------------------------------------------------------
// StrokeParams truncation
// ---------------------------------------------------------------------------

TEST(CodecTest, StrokeParamsTruncatedFails) {
  WireWriter w;
  w.writeF64(2.0);  // strokeWidth only.
  WireReader r(w.data());
  StrokeParams out;
  EXPECT_FALSE(DecodeStrokeParams(r, out));
}

// ---------------------------------------------------------------------------
// WireGradient (linear + radial)
// ---------------------------------------------------------------------------

namespace {

WireGradient MakeLinearGradient() {
  WireGradient g;
  g.kind = WireGradient::Kind::kLinear;
  g.units = GradientUnits::UserSpaceOnUse;
  g.spreadMethod = GradientSpreadMethod::Repeat;
  GradientStop s0;
  s0.offset = 0.0f;
  s0.color = css::Color(css::RGBA(255, 0, 0, 255));
  s0.opacity = 1.0f;
  GradientStop s1;
  s1.offset = 1.0f;
  s1.color = css::Color(css::RGBA(0, 0, 255, 255));
  s1.opacity = 0.5f;
  g.stops = {s0, s1};
  g.x1 = Lengthd(0, LengthUnit::Px);
  g.y1 = Lengthd(0, LengthUnit::Px);
  g.x2 = Lengthd(100, LengthUnit::Px);
  g.y2 = Lengthd(0, LengthUnit::Px);
  g.cx = Lengthd(50, LengthUnit::Px);
  g.cy = Lengthd(50, LengthUnit::Px);
  g.r = Lengthd(50, LengthUnit::Px);
  g.fr = Lengthd(0, LengthUnit::Px);
  g.fallback = css::Color(css::RGBA(1, 2, 3, 4));
  return g;
}

}  // namespace

TEST(CodecTest, WireGradientLinearRoundTrip) {
  const WireGradient g = MakeLinearGradient();
  WireWriter w;
  EncodeWireGradient(w, g);
  WireReader r(w.data());
  WireGradient out;
  ASSERT_TRUE(DecodeWireGradient(r, out));
  EXPECT_EQ(out.kind, WireGradient::Kind::kLinear);
  EXPECT_EQ(out.units, GradientUnits::UserSpaceOnUse);
  EXPECT_EQ(out.spreadMethod, GradientSpreadMethod::Repeat);
  ASSERT_EQ(out.stops.size(), 2u);
  EXPECT_FLOAT_EQ(out.stops[0].offset, 0.0f);
  EXPECT_FLOAT_EQ(out.stops[1].opacity, 0.5f);
  EXPECT_THAT(out.x2, LengthdEq(100.0, LengthUnit::Px));
  EXPECT_FALSE(out.fx.has_value());
  EXPECT_FALSE(out.fy.has_value());
  ASSERT_TRUE(out.fallback.has_value());
  ASSERT_TRUE(std::holds_alternative<css::RGBA>(out.fallback->value));
  EXPECT_EQ(std::get<css::RGBA>(out.fallback->value), css::RGBA(1, 2, 3, 4));
  EXPECT_EQ(r.remaining(), 0u);
}

TEST(CodecTest, WireGradientRadialWithFociRoundTrip) {
  WireGradient g = MakeLinearGradient();
  g.kind = WireGradient::Kind::kRadial;
  g.units = GradientUnits::ObjectBoundingBox;
  g.fx = Lengthd(10, LengthUnit::Px);
  g.fy = Lengthd(20, LengthUnit::Px);
  g.fr = Lengthd(5, LengthUnit::Px);
  g.fallback.reset();

  WireWriter w;
  EncodeWireGradient(w, g);
  WireReader r(w.data());
  WireGradient out;
  ASSERT_TRUE(DecodeWireGradient(r, out));
  EXPECT_EQ(out.kind, WireGradient::Kind::kRadial);
  ASSERT_TRUE(out.fx.has_value());
  EXPECT_THAT(*out.fx, LengthdEq(10.0, LengthUnit::Px));
  ASSERT_TRUE(out.fy.has_value());
  EXPECT_THAT(*out.fy, LengthdEq(20.0, LengthUnit::Px));
  EXPECT_THAT(out.fr, LengthdEq(5.0, LengthUnit::Px));
  EXPECT_FALSE(out.fallback.has_value());
}

TEST(CodecTest, WireGradientRejectsInvalidKind) {
  WireWriter w;
  w.writeU8(0xFF);  // invalid kind.
  WireReader r(w.data());
  WireGradient out;
  EXPECT_FALSE(DecodeWireGradient(r, out));
  EXPECT_TRUE(r.failed());
}

TEST(CodecTest, WireGradientTruncatedFails) {
  WireWriter w;  // empty.
  WireReader r(w.data());
  WireGradient out;
  EXPECT_FALSE(DecodeWireGradient(r, out));
}

// ---------------------------------------------------------------------------
// ResolvedPaintServer: every variant tag
// ---------------------------------------------------------------------------

TEST(CodecTest, ResolvedPaintServerNoneRoundTrip) {
  svg::components::ResolvedPaintServer p = PaintServer::None{};
  WireWriter w;
  EncodeResolvedPaintServer(w, p);
  WireReader r(w.data());
  svg::components::ResolvedPaintServer out = PaintServer::Solid(css::Color(css::RGBA{}));
  ASSERT_TRUE(DecodeResolvedPaintServer(r, out));
  EXPECT_TRUE(std::holds_alternative<PaintServer::None>(out));
}

TEST(CodecTest, ResolvedPaintServerSolidRoundTrip) {
  svg::components::ResolvedPaintServer p =
      PaintServer::Solid(css::Color(css::RGBA(10, 20, 30, 40)));
  WireWriter w;
  EncodeResolvedPaintServer(w, p);
  WireReader r(w.data());
  svg::components::ResolvedPaintServer out = PaintServer::None{};
  ASSERT_TRUE(DecodeResolvedPaintServer(r, out));
  ASSERT_TRUE(std::holds_alternative<PaintServer::Solid>(out));
  EXPECT_EQ(std::get<PaintServer::Solid>(out).color.value,
            css::Color::Type(css::RGBA(10, 20, 30, 40)));
}

TEST(CodecTest, ResolvedPaintServerStubDecodesToTransparentSolid) {
  // A hand-built stub tag (2) must decode to a transparent solid, per the
  // pattern side-channel contract.
  WireWriter w;
  w.writeU8(2);  // kPaintTagStub
  WireReader r(w.data());
  svg::components::ResolvedPaintServer out = PaintServer::None{};
  ASSERT_TRUE(DecodeResolvedPaintServer(r, out));
  ASSERT_TRUE(std::holds_alternative<PaintServer::Solid>(out));
  EXPECT_EQ(std::get<PaintServer::Solid>(out).color.value, css::Color::Type(css::RGBA(0, 0, 0, 0)));
}

TEST(CodecTest, ResolvedPaintServerGradientStashedInPending) {
  // Build a gradient-tagged paint server by hand (tag 3 + WireGradient).
  WireWriter w;
  w.writeU8(3);  // kPaintTagGradient
  EncodeWireGradient(w, MakeLinearGradient());
  WireReader r(w.data());
  svg::components::ResolvedPaintServer out = PaintServer::Solid(css::Color(css::RGBA{}));
  std::optional<WireGradient> pending;
  ASSERT_TRUE(DecodeResolvedPaintServer(r, out, &pending));
  // The variant becomes None; the gradient is stashed for the replayer.
  EXPECT_TRUE(std::holds_alternative<PaintServer::None>(out));
  ASSERT_TRUE(pending.has_value());
  EXPECT_EQ(pending->kind, WireGradient::Kind::kLinear);
}

TEST(CodecTest, ResolvedPaintServerRejectsUnknownTag) {
  WireWriter w;
  w.writeU8(0xFF);
  WireReader r(w.data());
  svg::components::ResolvedPaintServer out = PaintServer::None{};
  EXPECT_FALSE(DecodeResolvedPaintServer(r, out));
  EXPECT_TRUE(r.failed());
}

TEST(CodecTest, ResolvedPaintServerTruncatedFails) {
  WireWriter w;  // no tag byte.
  WireReader r(w.data());
  svg::components::ResolvedPaintServer out = PaintServer::None{};
  EXPECT_FALSE(DecodeResolvedPaintServer(r, out));
}

// ---------------------------------------------------------------------------
// PaintParams with gradient stashing
// ---------------------------------------------------------------------------

TEST(CodecTest, PaintParamsGradientFillStashed) {
  PaintParams p;
  p.opacity = 0.6;
  // Encode a gradient fill by hand so the decode path stashes it.
  WireWriter w;
  w.writeF64(p.opacity);
  w.writeU8(3);  // fill: gradient
  EncodeWireGradient(w, MakeLinearGradient());
  w.writeU8(0);                                         // stroke: none
  w.writeF64(0.9);                                      // fillOpacity
  w.writeF64(1.0);                                      // strokeOpacity
  EncodeColor(w, css::Color(css::RGBA(7, 7, 7, 255)));  // currentColor
  EncodeBox2d(w, Box2d(Vector2d(0, 0), Vector2d(50, 50)));
  EncodeStrokeParams(w, StrokeParams{});

  WireReader r(w.data());
  PaintParams out;
  std::optional<WireGradient> fillG, strokeG;
  ASSERT_TRUE(DecodePaintParams(r, out, &fillG, &strokeG));
  EXPECT_DOUBLE_EQ(out.opacity, 0.6);
  EXPECT_DOUBLE_EQ(out.fillOpacity, 0.9);
  ASSERT_TRUE(fillG.has_value());
  EXPECT_FALSE(strokeG.has_value());
}

TEST(CodecTest, PaintParamsTruncatedFails) {
  WireWriter w;  // empty.
  WireReader r(w.data());
  PaintParams out;
  EXPECT_FALSE(DecodePaintParams(r, out));
}

// ---------------------------------------------------------------------------
// ResolvedClip with paths
// ---------------------------------------------------------------------------

TEST(CodecTest, ResolvedClipWithPathsRoundTrip) {
  ResolvedClip c;
  c.clipRect = Box2d(Vector2d(1, 2), Vector2d(3, 4));
  PathShape shape;
  PathBuilder pb;
  pb.moveTo(Vector2d(0, 0)).lineTo(Vector2d(5, 5)).closePath();
  shape.path = pb.build();
  shape.layer = 2;
  c.clipPaths.push_back(shape);
  c.clipPathUnitsTransform = Transform2d::Scale(Vector2d(2, 2));

  WireWriter w;
  EncodeResolvedClip(w, c);
  WireReader r(w.data());
  ResolvedClip out;
  ASSERT_TRUE(DecodeResolvedClip(r, out));
  ASSERT_TRUE(out.clipRect.has_value());
  EXPECT_THAT(*out.clipRect, Box2dCorners(1.0, 2.0, 3.0, 4.0));
  ASSERT_EQ(out.clipPaths.size(), 1u);
  EXPECT_EQ(out.clipPaths[0].layer, 2);
  EXPECT_FALSE(out.mask.has_value());
}

TEST(CodecTest, ResolvedClipRejectsPresentMask) {
  // Hand-build a clip stream whose trailing mask-present flag is true, which
  // the encoder never emits — the decoder must treat it as corruption.
  WireWriter w;
  w.writeBool(false);  // no clipRect
  w.writeU32(0);       // no clipPaths
  EncodeTransform2d(w, Transform2d());
  w.writeBool(true);  // mask present — illegal.
  WireReader r(w.data());
  ResolvedClip out;
  EXPECT_FALSE(DecodeResolvedClip(r, out));
  EXPECT_TRUE(r.failed());
}

TEST(CodecTest, ResolvedClipTruncatedFails) {
  WireWriter w;  // empty.
  WireReader r(w.data());
  ResolvedClip out;
  EXPECT_FALSE(DecodeResolvedClip(r, out));
}

// ---------------------------------------------------------------------------
// RenderViewport
// ---------------------------------------------------------------------------

TEST(CodecTest, RenderViewportRoundTrip) {
  RenderViewport vp;
  vp.size = Vector2d(640, 480);
  vp.devicePixelRatio = 2.0;
  WireWriter w;
  EncodeRenderViewport(w, vp);
  WireReader r(w.data());
  RenderViewport out;
  ASSERT_TRUE(DecodeRenderViewport(r, out));
  EXPECT_DOUBLE_EQ(out.size.x, 640);
  EXPECT_DOUBLE_EQ(out.size.y, 480);
  EXPECT_DOUBLE_EQ(out.devicePixelRatio, 2.0);
}

TEST(CodecTest, RenderViewportTruncatedFails) {
  WireWriter w;
  EncodeVector2d(w, Vector2d(1, 1));  // size present, devicePixelRatio missing.
  WireReader r(w.data());
  RenderViewport out;
  EXPECT_FALSE(DecodeRenderViewport(r, out));
}

// ---------------------------------------------------------------------------
// ImageParams
// ---------------------------------------------------------------------------

TEST(CodecTest, ImageParamsRoundTrip) {
  ImageParams p;
  p.targetRect = Box2d(Vector2d(5, 6), Vector2d(105, 106));
  p.opacity = 0.42;
  p.imageRenderingPixelated = true;
  WireWriter w;
  EncodeImageParams(w, p);
  WireReader r(w.data());
  ImageParams out;
  ASSERT_TRUE(DecodeImageParams(r, out));
  EXPECT_THAT(out.targetRect, Box2dCorners(5.0, 6.0, 105.0, 106.0));
  EXPECT_DOUBLE_EQ(out.opacity, 0.42);
  EXPECT_TRUE(out.imageRenderingPixelated);
}

TEST(CodecTest, ImageParamsTruncatedFails) {
  WireWriter w;  // empty.
  WireReader r(w.data());
  ImageParams out;
  EXPECT_FALSE(DecodeImageParams(r, out));
}

// ---------------------------------------------------------------------------
// ImageResource
// ---------------------------------------------------------------------------

TEST(CodecTest, ImageResourceRoundTrip) {
  ImageResource img;
  img.width = 2;
  img.height = 1;
  img.data = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
  WireWriter w;
  EncodeImageResource(w, img);
  WireReader r(w.data());
  ImageResource out;
  ASSERT_TRUE(DecodeImageResource(r, out));
  EXPECT_EQ(out.width, 2);
  EXPECT_EQ(out.height, 1);
  EXPECT_THAT(out.data, ElementsAre(0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04));
}

TEST(CodecTest, ImageResourceEmptyDataRoundTrip) {
  ImageResource img;
  img.width = 0;
  img.height = 0;
  WireWriter w;
  EncodeImageResource(w, img);
  WireReader r(w.data());
  ImageResource out;
  ASSERT_TRUE(DecodeImageResource(r, out));
  EXPECT_TRUE(out.data.empty());
}

TEST(CodecTest, ImageResourceRejectsNegativeDimensions) {
  WireWriter w;
  w.writeI32(-1);  // negative width.
  w.writeI32(4);
  w.writeU32(0);
  WireReader r(w.data());
  ImageResource out;
  EXPECT_FALSE(DecodeImageResource(r, out));
  EXPECT_TRUE(r.failed());
}

TEST(CodecTest, ImageResourceTruncatedDataFails) {
  WireWriter w;
  w.writeI32(2);
  w.writeI32(2);
  w.writeU32(16);  // claims 16 bytes of pixel data but provides none.
  WireReader r(w.data());
  ImageResource out;
  EXPECT_FALSE(DecodeImageResource(r, out));
}

// ---------------------------------------------------------------------------
// ComputedTextComponent
// ---------------------------------------------------------------------------

TEST(CodecTest, ComputedTextComponentRoundTrip) {
  svg::components::ComputedTextComponent text;
  svg::components::ComputedTextComponent::TextSpan span;
  span.text = RcString("Hello");
  span.start = 0;
  span.end = 5;
  span.xList = {Lengthd(1, LengthUnit::Px), std::nullopt};
  span.rotateList = {15.0, 30.0};
  span.fontSize = Lengthd(16, LengthUnit::Px);
  span.fontWeight = 700;
  span.opacity = 0.9;
  text.spans.push_back(span);

  WireWriter w;
  EncodeComputedTextComponent(w, text);
  WireReader r(w.data());
  svg::components::ComputedTextComponent out;
  ASSERT_TRUE(DecodeComputedTextComponent(r, out));
  ASSERT_EQ(out.spans.size(), 1u);
  EXPECT_EQ(std::string_view(out.spans[0].text), "Hello");
  EXPECT_EQ(out.spans[0].start, 0u);
  EXPECT_EQ(out.spans[0].end, 5u);
  EXPECT_EQ(out.spans[0].fontWeight, 700);
  ASSERT_EQ(out.spans[0].xList.size(), 2u);
  EXPECT_TRUE(out.spans[0].xList[0].has_value());
  EXPECT_FALSE(out.spans[0].xList[1].has_value());
  ASSERT_EQ(out.spans[0].rotateList.size(), 2u);
  EXPECT_THAT(out.spans[0].rotateList[1], DoubleEq(30.0));
  EXPECT_EQ(r.remaining(), 0u);
}

TEST(CodecTest, ComputedTextComponentEmptyRoundTrip) {
  svg::components::ComputedTextComponent text;
  WireWriter w;
  EncodeComputedTextComponent(w, text);
  WireReader r(w.data());
  svg::components::ComputedTextComponent out;
  ASSERT_TRUE(DecodeComputedTextComponent(r, out));
  EXPECT_TRUE(out.spans.empty());
}

TEST(CodecTest, ComputedTextComponentTruncatedFails) {
  WireWriter w;  // empty: can't read span count.
  WireReader r(w.data());
  svg::components::ComputedTextComponent out;
  EXPECT_FALSE(DecodeComputedTextComponent(r, out));
}

// ---------------------------------------------------------------------------
// TextParams without font faces
// ---------------------------------------------------------------------------

TEST(CodecTest, TextParamsNoFontFacesRoundTrip) {
  svg::TextParams params;
  params.opacity = 0.7;
  params.fillColor = css::Color(css::RGBA(11, 22, 33, 255));
  params.fontFamilies.push_back(RcString("Serif"));
  params.fontSize = Lengthd(14, LengthUnit::Px);
  params.textLength = Lengthd(200, LengthUnit::Px);

  WireWriter w;
  EncodeTextParams(w, params);
  WireReader r(w.data());
  svg::TextParams out;
  // No outFontFaces pointer — decoder must still consume the (empty) face list.
  ASSERT_TRUE(DecodeTextParams(r, out));
  EXPECT_DOUBLE_EQ(out.opacity, 0.7);
  ASSERT_EQ(out.fontFamilies.size(), 1u);
  EXPECT_EQ(std::string_view(out.fontFamilies[0]), "Serif");
  EXPECT_THAT(out.fontSize, LengthdEq(14.0, LengthUnit::Px));
  ASSERT_TRUE(out.textLength.has_value());
  EXPECT_THAT(*out.textLength, LengthdEq(200.0, LengthUnit::Px));
  EXPECT_EQ(r.remaining(), 0u);
}

TEST(CodecTest, TextParamsTruncatedFails) {
  WireWriter w;  // empty.
  WireReader r(w.data());
  svg::TextParams out;
  EXPECT_FALSE(DecodeTextParams(r, out));
}

TEST(CodecTest, FontFaceTruncatedFails) {
  WireWriter w;  // empty: can't read family name length.
  WireReader r(w.data());
  css::FontFace out;
  EXPECT_FALSE(DecodeFontFace(r, out));
}

// ---------------------------------------------------------------------------
// FilterGraph: empty + populated with representative primitives
// ---------------------------------------------------------------------------

TEST(CodecTest, FilterGraphEmptyRoundTrip) {
  svg::components::FilterGraph g;
  g.userToPixelScale = Vector2d(1.5, 2.5);
  WireWriter w;
  EncodeFilterGraph(w, g);
  WireReader r(w.data());
  svg::components::FilterGraph out;
  ASSERT_TRUE(DecodeFilterGraph(r, out));
  EXPECT_TRUE(out.nodes.empty());
  EXPECT_DOUBLE_EQ(out.userToPixelScale.x, 1.5);
  EXPECT_DOUBLE_EQ(out.userToPixelScale.y, 2.5);
  EXPECT_EQ(r.remaining(), 0u);
}

TEST(CodecTest, FilterGraphWithPrimitivesRoundTrip) {
  namespace fp = svg::components::filter_primitive;
  svg::components::FilterGraph g;

  // Node 0: GaussianBlur with a named result + standard input + subregion.
  {
    svg::components::FilterNode node;
    fp::GaussianBlur blur;
    blur.stdDeviationX = 3.0;
    blur.stdDeviationY = 4.0;
    node.primitive = blur;
    node.inputs.push_back(
        svg::components::FilterInput(svg::components::FilterStandardInput::SourceGraphic));
    node.result = RcString("blurred");
    node.x = Lengthd(1, LengthUnit::Px);
    node.colorInterpolationFilters = svg::ColorInterpolationFilters::SRGB;
    g.nodes.push_back(std::move(node));
  }
  // Node 1: ColorMatrix carrying a values vector + a named input.
  {
    svg::components::FilterNode node;
    fp::ColorMatrix cm;
    cm.type = fp::ColorMatrix::Type::Matrix;
    cm.values = {0.1, 0.2, 0.3, 0.4, 0.5};
    node.primitive = cm;
    node.inputs.push_back(
        svg::components::FilterInput(svg::components::FilterInput::Named{RcString("blurred")}));
    g.nodes.push_back(std::move(node));
  }
  // Node 2: Flood with a color + Previous (implicit) input.
  {
    svg::components::FilterNode node;
    fp::Flood flood;
    flood.floodColor = css::Color(css::RGBA(255, 128, 0, 255));
    flood.floodOpacity = 0.75;
    node.primitive = flood;
    node.inputs.push_back(svg::components::FilterInput(svg::components::FilterInput::Previous{}));
    g.nodes.push_back(std::move(node));
  }

  g.colorInterpolationFilters = svg::ColorInterpolationFilters::LinearRGB;
  g.elementBoundingBox = Box2d(Vector2d(0, 0), Vector2d(80, 80));
  g.filterRegion = Box2d(Vector2d(-8, -8), Vector2d(88, 88));
  g.userToPixelScale = Vector2d(2, 2);

  WireWriter w;
  EncodeFilterGraph(w, g);
  WireReader r(w.data());
  svg::components::FilterGraph out;
  ASSERT_TRUE(DecodeFilterGraph(r, out));
  ASSERT_EQ(out.nodes.size(), 3u);

  ASSERT_TRUE(std::holds_alternative<fp::GaussianBlur>(out.nodes[0].primitive));
  EXPECT_DOUBLE_EQ(std::get<fp::GaussianBlur>(out.nodes[0].primitive).stdDeviationX, 3.0);
  ASSERT_TRUE(out.nodes[0].result.has_value());
  EXPECT_EQ(std::string_view(*out.nodes[0].result), "blurred");
  ASSERT_EQ(out.nodes[0].inputs.size(), 1u);
  ASSERT_TRUE(out.nodes[0].x.has_value());

  ASSERT_TRUE(std::holds_alternative<fp::ColorMatrix>(out.nodes[1].primitive));
  EXPECT_THAT(
      std::get<fp::ColorMatrix>(out.nodes[1].primitive).values,
      ElementsAre(DoubleEq(0.1), DoubleEq(0.2), DoubleEq(0.3), DoubleEq(0.4), DoubleEq(0.5)));

  ASSERT_TRUE(std::holds_alternative<fp::Flood>(out.nodes[2].primitive));
  EXPECT_DOUBLE_EQ(std::get<fp::Flood>(out.nodes[2].primitive).floodOpacity, 0.75);

  ASSERT_TRUE(out.elementBoundingBox.has_value());
  EXPECT_THAT(*out.elementBoundingBox, Box2dCorners(0.0, 0.0, 80.0, 80.0));
  ASSERT_TRUE(out.filterRegion.has_value());
  EXPECT_THAT(*out.filterRegion, Box2dCorners(-8.0, -8.0, 88.0, 88.0));
  EXPECT_EQ(r.remaining(), 0u);
}

TEST(CodecTest, FilterGraphRejectsInvalidPrimitiveTag) {
  WireWriter w;
  w.writeU32(1);    // one node
  w.writeU8(0xFF);  // invalid primitive tag.
  WireReader r(w.data());
  svg::components::FilterGraph out;
  EXPECT_FALSE(DecodeFilterGraph(r, out));
  EXPECT_TRUE(r.failed());
}

TEST(CodecTest, FilterGraphTruncatedFails) {
  WireWriter w;  // empty: can't read node count.
  WireReader r(w.data());
  svg::components::FilterGraph out;
  EXPECT_FALSE(DecodeFilterGraph(r, out));
}

}  // namespace
}  // namespace donner::editor::sandbox
