/// @file
/// Behavior of the tiny-skia backend's retained rasterization.
///
/// Two properties are load-bearing and both are asserted directly rather than inferred. First,
/// a retained frame is byte-identical to the frame a renderer that never retained anything
/// would have produced, checked frame by frame over documents covering the retained draw kinds
/// and the bypassed ones. Second, replay only happens when it is sound: every class of change
/// that could alter a shape's pixels is mutated in isolation and the frame after it must be
/// produced by rasterizing, which the counters make observable.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererTinySkia.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

SVGDocument parseDocument(std::string_view svg) {
  ParseWarningSink warningSink;
  auto parsed = parser::SVGParser::ParseSVG(svg, warningSink);
  EXPECT_FALSE(parsed.hasError()) << parsed.error();
  return std::move(parsed).result();
}

/// Wraps a fragment in a fixed-size document, so a test can describe only the shapes it cares
/// about.
SVGDocument parseFragment(std::string_view fragment, int width = 96, int height = 96) {
  std::ostringstream svg;
  svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" "
      << "xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"" << width << "\" height=\""
      << height << "\">" << fragment << "</svg>";
  return parseDocument(svg.str());
}

/// Renders `document` `frames` times through one renderer and returns every frame.
std::vector<RendererBitmap> renderFrames(SVGDocument& document, int frames, bool retained) {
  RendererTinySkia renderer;
  renderer.setRetainedSpansEnabled(retained);

  std::vector<RendererBitmap> bitmaps;
  bitmaps.reserve(static_cast<std::size_t>(frames));
  for (int i = 0; i < frames; ++i) {
    renderer.draw(document);
    bitmaps.push_back(renderer.takeSnapshot());
  }
  return bitmaps;
}

::testing::AssertionResult BitmapsEqual(const RendererBitmap& lhs, const RendererBitmap& rhs) {
  if (lhs.dimensions != rhs.dimensions || lhs.rowBytes != rhs.rowBytes) {
    return ::testing::AssertionFailure()
           << "dimensions differ: " << lhs.dimensions << " (row " << lhs.rowBytes << ") vs "
           << rhs.dimensions << " (row " << rhs.rowBytes << ")";
  }
  if (lhs.pixels.size() != rhs.pixels.size()) {
    return ::testing::AssertionFailure()
           << "pixel buffer sizes differ: " << lhs.pixels.size() << " vs " << rhs.pixels.size();
  }

  for (std::size_t i = 0; i < lhs.pixels.size(); ++i) {
    if (lhs.pixels[i] != rhs.pixels[i]) {
      const std::size_t pixel = i / 4;
      const int x = static_cast<int>(pixel % static_cast<std::size_t>(lhs.dimensions.x));
      const int y = static_cast<int>(pixel / static_cast<std::size_t>(lhs.dimensions.x));
      return ::testing::AssertionFailure()
             << "first difference at pixel (" << x << ", " << y << ") channel " << (i % 4) << ": "
             << static_cast<int>(lhs.pixels[i]) << " vs " << static_cast<int>(rhs.pixels[i]);
    }
  }
  return ::testing::AssertionSuccess();
}

/// Renders the same document with and without retention and compares every frame.
void ExpectRetainedFramesMatchFresh(SVGDocument& document, int frames = 3) {
  const std::vector<RendererBitmap> fresh = renderFrames(document, frames, /*retained=*/false);
  const std::vector<RendererBitmap> retained = renderFrames(document, frames, /*retained=*/true);
  ASSERT_EQ(fresh.size(), retained.size());
  for (std::size_t i = 0; i < fresh.size(); ++i) {
    EXPECT_TRUE(BitmapsEqual(fresh[i], retained[i])) << "frame " << i;
  }
}

/// Renders `document` once through a fresh renderer, which is the reference every retained
/// frame is measured against.
RendererBitmap renderFresh(SVGDocument& document) {
  RendererTinySkia renderer;
  renderer.draw(document);
  return renderer.takeSnapshot();
}

std::string readTestFile(const char* path) {
  std::ifstream file(path, std::ios::binary);
  EXPECT_TRUE(file.good()) << "unable to open " << path;
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

constexpr std::string_view kShapes = R"svg(
    <rect x="4" y="4" width="40" height="30" fill="#ff0000"/>
    <circle cx="70" cy="20" r="16" fill="#00a000" stroke="#000080" stroke-width="3"/>
    <path d="M 8 50 L 50 90 L 8 90 Z" fill="#3040ff" fill-opacity="0.6"/>
    <path d="M 55 55 C 70 45, 90 70, 60 88" fill="none" stroke="#804000" stroke-width="5"/>
  )svg";

}  // namespace

TEST(RendererRetainedSpans, UnchangedFrameReplaysInsteadOfRasterizing) {
  SVGDocument document = parseFragment(kShapes);

  RendererTinySkia renderer;
  renderer.setRetainedSpansEnabled(true);

  renderer.draw(document);
  const RendererBitmap first = renderer.takeSnapshot();
  const RetainedSpanStats afterFirst = renderer.retainedSpanStats();
  EXPECT_GT(afterFirst.capturedDraws, 0u);
  EXPECT_EQ(afterFirst.replayedDraws, 0u);

  renderer.draw(document);
  const RendererBitmap second = renderer.takeSnapshot();
  const RetainedSpanStats afterSecond = renderer.retainedSpanStats();

  EXPECT_EQ(afterSecond.replayedDraws, afterFirst.capturedDraws)
      << "every draw captured on the first frame must replay on an unchanged second frame";
  EXPECT_EQ(afterSecond.capturedDraws, 0u) << "an unchanged frame must not rasterize anything";
  EXPECT_EQ(afterSecond.invalidatedDraws, 0u);
  EXPECT_EQ(afterSecond.refusedReplays, 0u);
  EXPECT_TRUE(BitmapsEqual(first, second));
  EXPECT_GT(afterSecond.liveBytes, 0u);
}

TEST(RendererRetainedSpans, RetentionIsOffByDefault) {
  SVGDocument document = parseFragment(kShapes);

  RendererTinySkia renderer;
  EXPECT_FALSE(renderer.retainedSpansEnabled());

  renderer.draw(document);
  renderer.draw(document);
  const RetainedSpanStats stats = renderer.retainedSpanStats();
  EXPECT_EQ(stats.capturedDraws, 0u);
  EXPECT_EQ(stats.replayedDraws, 0u);
  EXPECT_EQ(stats.liveBytes, 0u);
}

/// The frame-by-frame A/B: every one of these documents renders identically with and without
/// retention, on the capture frame and on every replay frame after it.
TEST(RendererRetainedSpans, FramesAreByteIdenticalToFreshRendering) {
  struct Case {
    const char* name;
    std::string_view fragment;
  };

  const Case cases[] = {
      {"shapes", kShapes},
      {"even_odd_fill",
       R"svg(<path d="M 10 10 H 80 V 80 H 10 Z M 30 30 H 60 V 60 H 30 Z" fill-rule="evenodd"
            fill="#204080"/>)svg"},
      {"hairline_stroke",
       R"svg(<path d="M 5 5 L 90 90" fill="none" stroke="#000000" stroke-width="0.2"/>)svg"},
      {"dashed_stroke",
       R"svg(<path d="M 5 40 H 90" fill="none" stroke="#c00000" stroke-width="6"
            stroke-dasharray="9 4" stroke-dashoffset="2"/>)svg"},
      {"zero_gap_dash",
       R"svg(<rect x="10" y="10" width="60" height="60" fill="none" stroke="#008080"
            stroke-width="4" stroke-dasharray="8 0"/>)svg"},
      {"round_join_stroke",
       R"svg(<path d="M 10 80 L 45 10 L 80 80" fill="none" stroke="#602080" stroke-width="12"
            stroke-linejoin="round" stroke-linecap="round"/>)svg"},
      {"linear_gradient",
       R"svg(<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1">
            <stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/>
          </linearGradient></defs>
          <rect x="5" y="5" width="86" height="86" fill="url(#g)"/>)svg"},
      {"radial_gradient",
       R"svg(<defs><radialGradient id="g" cx="0.4" cy="0.4" r="0.6">
            <stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#004000"/>
          </radialGradient></defs>
          <circle cx="48" cy="48" r="40" fill="url(#g)"/>)svg"},
      {"gradient_stroke",
       R"svg(<defs><linearGradient id="g"><stop offset="0" stop-color="#ff8000"/>
            <stop offset="1" stop-color="#0080ff"/></linearGradient></defs>
          <path d="M 10 48 H 86" fill="none" stroke="url(#g)" stroke-width="14"/>)svg"},
      {"clipped_shapes",
       R"svg(<defs><clipPath id="c"><circle cx="48" cy="48" r="30"/></clipPath></defs>
          <g clip-path="url(#c)"><rect x="0" y="0" width="96" height="96" fill="#802020"/>
          <path d="M 0 96 L 96 0 L 96 96 Z" fill="#208020"/></g>)svg"},
      {"nested_clips",
       R"svg(<defs><clipPath id="a"><rect x="10" y="10" width="70" height="70"/></clipPath>
          <clipPath id="b"><circle cx="48" cy="48" r="28"/></clipPath></defs>
          <g clip-path="url(#a)"><g clip-path="url(#b)">
            <rect x="0" y="0" width="96" height="96" fill="#3060c0"/></g></g>)svg"},
      {"group_opacity",
       R"svg(<g opacity="0.5"><rect x="10" y="10" width="50" height="50" fill="#ff0000"/>
          <rect x="30" y="30" width="50" height="50" fill="#0000ff"/></g>)svg"},
      {"nested_transforms",
       R"svg(<g transform="translate(20 10) rotate(15)"><g transform="scale(1.4)">
          <rect x="0" y="0" width="30" height="30" fill="#008040"/></g></g>)svg"},
      {"pattern_fill",
       R"svg(<defs><pattern id="p" width="12" height="12" patternUnits="userSpaceOnUse">
            <rect width="6" height="6" fill="#c04000"/>
          </pattern></defs>
          <rect x="5" y="5" width="86" height="86" fill="url(#p)"/>)svg"},
      {"instanced_use",
       R"svg(<defs><path id="s" d="M 0 0 H 30 V 30 H 0 Z" fill="#a02060"/></defs>
          <use xlink:href="#s" x="5" y="5"/><use xlink:href="#s" x="50" y="50"/>)svg"},
      {"paint_order",
       R"svg(<path d="M 20 20 H 76 V 76 H 20 Z" fill="#ffd000" stroke="#202020" stroke-width="10"
            paint-order="stroke fill"/>)svg"},
      {"mask",
       R"svg(<defs><mask id="m"><rect x="0" y="0" width="96" height="48" fill="#ffffff"/></mask>
          </defs><rect x="0" y="0" width="96" height="96" fill="#0040ff" mask="url(#m)"/>)svg"},
      {"markers",
       R"svg(<defs><marker id="m" markerWidth="6" markerHeight="6" refX="3" refY="3">
            <circle cx="3" cy="3" r="3" fill="#c00060"/></marker></defs>
          <path d="M 10 10 L 50 50 L 86 20" fill="none" stroke="#202020" stroke-width="2"
            marker-start="url(#m)" marker-mid="url(#m)" marker-end="url(#m)"/>)svg"},
      {"empty_document", R"svg(<g/>)svg"},
  };

  for (const Case& testCase : cases) {
    SCOPED_TRACE(testCase.name);
    SVGDocument document = parseFragment(testCase.fragment);
    ExpectRetainedFramesMatchFresh(document, /*frames=*/4);
  }
}

TEST(RendererRetainedSpans, RealDocumentFramesAreByteIdenticalToFreshRendering) {
  const char* paths[] = {
      "donner/svg/renderer/testdata/Ghostscript_Tiger.svg",
      "donner/svg/renderer/testdata/ellipse1.svg",
      "donner/svg/renderer/testdata/a-fill-rule-001.svg",
  };

  for (const char* path : paths) {
    SCOPED_TRACE(path);
    const std::string source = readTestFile(path);
    ASSERT_FALSE(source.empty());
    SVGDocument document = parseDocument(source);
    ExpectRetainedFramesMatchFresh(document, /*frames=*/3);
  }
}

TEST(RendererRetainedSpans, TigerSteadyFrameReplaysEveryDraw) {
  const std::string source = readTestFile("donner/svg/renderer/testdata/Ghostscript_Tiger.svg");
  ASSERT_FALSE(source.empty());
  SVGDocument document = parseDocument(source);

  RendererTinySkia renderer;
  renderer.setRetainedSpansEnabled(true);
  renderer.draw(document);
  const RetainedSpanStats afterFirst = renderer.retainedSpanStats();
  ASSERT_GT(afterFirst.capturedDraws, 100u) << "the tiger is hundreds of filled and stroked paths";
  EXPECT_EQ(afterFirst.bypassedDraws, 0u) << "every tiger draw is a retainable root-surface draw";

  renderer.draw(document);
  const RetainedSpanStats afterSecond = renderer.retainedSpanStats();
  EXPECT_EQ(afterSecond.capturedDraws, 0u);
  EXPECT_EQ(afterSecond.replayedDraws, afterFirst.capturedDraws);
}

namespace {

/// Renders two settled frames with retention on, applies `mutate`, then renders one more.
///
/// The returned stats describe that third frame, which is where an invalidation has to show up
/// as rasterization. The bitmap is compared against a renderer that never retained anything, so
/// the test fails both when a change is missed (stale pixels) and when it is applied wrongly.
struct MutationResult {
  RetainedSpanStats stats;
  RendererBitmap retained;
  RendererBitmap fresh;
};

template <typename MutateFn>
MutationResult renderThroughMutation(SVGDocument& document, MutateFn&& mutate) {
  RendererTinySkia renderer;
  renderer.setRetainedSpansEnabled(true);
  renderer.draw(document);
  renderer.draw(document);
  EXPECT_GT(renderer.retainedSpanStats().replayedDraws, 0u)
      << "the mutation test needs a populated cache to invalidate";

  mutate();

  renderer.draw(document);
  MutationResult result;
  result.stats = renderer.retainedSpanStats();
  result.retained = renderer.takeSnapshot();
  result.fresh = renderFresh(document);
  return result;
}

void ExpectRasterizedAndCorrect(const MutationResult& result) {
  EXPECT_GT(result.stats.capturedDraws, 0u)
      << "the changed shape must rasterize instead of replaying retained coverage";
  EXPECT_TRUE(BitmapsEqual(result.fresh, result.retained));
}

}  // namespace

TEST(RendererRetainedSpans, GeometryEditFallsBackToRasterizing) {
  SVGDocument document =
      parseFragment(R"svg(<path id="p" d="M 10 10 H 70 V 70 H 10 Z" fill="#20a020"/>)svg");
  auto path = document.querySelector("#p");
  ASSERT_TRUE(path.has_value());

  const MutationResult result =
      renderThroughMutation(document, [&] { path->setAttribute("d", "M 20 20 H 86 V 86 H 20 Z"); });
  ExpectRasterizedAndCorrect(result);
  EXPECT_EQ(result.stats.invalidatedDraws, 0u)
      << "a geometry change drops the whole entry, so the redraw is a fresh capture rather "
         "than a key mismatch";
}

TEST(RendererRetainedSpans, TransformChangeFallsBackToRasterizing) {
  SVGDocument document =
      parseFragment(R"svg(<rect id="r" x="10" y="10" width="40" height="40" fill="#a02020"/>)svg");
  auto rect = document.querySelector("#r");
  ASSERT_TRUE(rect.has_value());

  const MutationResult result = renderThroughMutation(
      document, [&] { rect->setAttribute("transform", "translate(20, 15)"); });
  ExpectRasterizedAndCorrect(result);
  EXPECT_GT(result.stats.invalidatedDraws, 0u)
      << "the geometry is unchanged, so the transform must be caught by the retained key";
}

TEST(RendererRetainedSpans, SolidPaintChangeFallsBackToRasterizing) {
  SVGDocument document =
      parseFragment(R"svg(<rect id="r" x="10" y="10" width="60" height="60" fill="#a02020"/>)svg");
  auto rect = document.querySelector("#r");
  ASSERT_TRUE(rect.has_value());

  const MutationResult result =
      renderThroughMutation(document, [&] { rect->setAttribute("fill", "#2020a0"); });
  ExpectRasterizedAndCorrect(result);
  EXPECT_GT(result.stats.invalidatedDraws, 0u);
}

TEST(RendererRetainedSpans, GradientStopChangeFallsBackToRasterizing) {
  SVGDocument document = parseFragment(
      R"svg(<defs><linearGradient id="g"><stop id="s0" offset="0" stop-color="#ff0000"/>
           <stop id="s1" offset="1" stop-color="#0000ff"/></linearGradient></defs>
         <rect x="5" y="5" width="86" height="86" fill="url(#g)"/>)svg");
  auto stop0 = document.querySelector("#s0");
  ASSERT_TRUE(stop0.has_value());

  const MutationResult result =
      renderThroughMutation(document, [&] { stop0->setAttribute("stop-color", "#00ff00"); });
  ExpectRasterizedAndCorrect(result);
  EXPECT_GT(result.stats.invalidatedDraws, 0u)
      << "a stop edit changes no geometry, so only the retained key's paint comparison can "
         "catch it";
}

TEST(RendererRetainedSpans, StrokeWidthChangeFallsBackToRasterizing) {
  SVGDocument document = parseFragment(
      R"svg(<path id="p" d="M 10 48 H 86" fill="none" stroke="#202080" stroke-width="4"/>)svg");
  auto path = document.querySelector("#p");
  ASSERT_TRUE(path.has_value());

  const MutationResult result =
      renderThroughMutation(document, [&] { path->setAttribute("stroke-width", "20"); });
  ExpectRasterizedAndCorrect(result);
  EXPECT_GT(result.stats.invalidatedDraws, 0u);
}

TEST(RendererRetainedSpans, ClipChangeFallsBackToRasterizing) {
  SVGDocument document = parseFragment(
      R"svg(<defs><clipPath id="c"><rect id="cr" x="0" y="0" width="48" height="96"/></clipPath>
          </defs>
         <rect x="0" y="0" width="96" height="96" fill="#206080" clip-path="url(#c)"/>)svg");
  auto clipRect = document.querySelector("#cr");
  ASSERT_TRUE(clipRect.has_value());

  const MutationResult result =
      renderThroughMutation(document, [&] { clipRect->setAttribute("width", "80"); });
  ExpectRasterizedAndCorrect(result);
  EXPECT_GT(result.stats.invalidatedDraws, 0u)
      << "the clipped shape's own geometry and paint are unchanged, so the clip identity in the "
         "retained key is what has to notice";
}

TEST(RendererRetainedSpans, SurfaceResizeFallsBackToRasterizing) {
  SVGDocument document =
      parseFragment(R"svg(<rect x="10" y="10" width="60" height="60" fill="#804020"/>)svg");

  RendererTinySkia renderer;
  renderer.setRetainedSpansEnabled(true);
  renderer.draw(document);
  renderer.draw(document);
  ASSERT_GT(renderer.retainedSpanStats().replayedDraws, 0u);

  document.setCanvasSize(140, 140);
  renderer.draw(document);
  const RetainedSpanStats stats = renderer.retainedSpanStats();
  const RendererBitmap resized = renderer.takeSnapshot();

  EXPECT_GT(stats.capturedDraws, 0u);
  EXPECT_EQ(resized.dimensions, Vector2i(140, 140));
  EXPECT_TRUE(BitmapsEqual(renderFresh(document), resized));
}

/// The surface binding is a memory-safety boundary, not bookkeeping: recorded runs are
/// device-space rectangles that nothing re-clips, so replaying them onto a smaller surface
/// reaches the blitter with an origin past the buffer. The retained key normally notices a
/// resize before the guard is reached, so this test drives the renderer's draw interface
/// directly, keeping every other input identical across two differently sized frames, and then
/// makes the key claim the recording came from the second frame's surface. The guard underneath
/// has to refuse anyway, and refusing has to draw the shape rather than drop it.
TEST(RendererRetainedSpans, ReplayRefusesRunsRecordedAgainstAnotherSurface) {
  SVGDocument document =
      parseFragment(R"svg(<rect id="r" x="10" y="10" width="60" height="60" fill="#3070c0"/>)svg");
  auto rect = document.querySelector("#r");
  ASSERT_TRUE(rect.has_value());
  const EntityHandle handle = rect->unsafeEntityHandle();

  const Path geometry = PathBuilder()
                            .moveTo(Vector2d(10, 10))
                            .lineTo(Vector2d(70, 10))
                            .lineTo(Vector2d(70, 70))
                            .lineTo(Vector2d(10, 70))
                            .closePath()
                            .build();
  PathShape shape;
  shape.path = &geometry;
  shape.sourceEntity = handle;

  PaintParams paint;
  paint.fill = PaintServer::Solid(css::Color(css::RGBA(0x30, 0x70, 0xC0, 0xFF)));
  paint.stroke = PaintServer::None();

  const auto drawOneFrame = [&](RendererTinySkia& renderer, int size) {
    RenderViewport viewport;
    viewport.size = Vector2d(size, size);
    renderer.beginFrame(viewport);
    renderer.setTransform(Transform2d());
    renderer.setPaint(paint);
    renderer.drawPath(shape, StrokeParams());
    renderer.endFrame();
  };

  RendererTinySkia renderer;
  renderer.setRetainedSpansEnabled(true);
  drawOneFrame(renderer, 140);
  ASSERT_EQ(renderer.retainedSpanStats().capturedDraws, 1u);

  Registry& registry = document.registry();
  ASSERT_TRUE(registry.all_of<RetainedSpansComponent>(handle.entity()));
  RetainedSpansComponent& retained = registry.get<RetainedSpansComponent>(handle.entity());
  ASSERT_TRUE(retained.fill.valid);
  // Everything else about the next frame's draw is identical by construction, so claiming the
  // recording came from that frame's surface leaves the recording itself as the only thing
  // standing between a 140-wide run and a 60-wide buffer.
  retained.fill.key.surfaceSize = tiny_skia::IntSize(60, 60);

  drawOneFrame(renderer, 60);
  const RetainedSpanStats stats = renderer.retainedSpanStats();
  const RendererBitmap shrunk = renderer.takeSnapshot();

  EXPECT_EQ(stats.refusedReplays, 1u) << "the surface-size guard must refuse the stale runs";
  EXPECT_EQ(stats.capturedDraws, 1u) << "a refusal must fall back to rasterizing, not to nothing";
  EXPECT_EQ(shrunk.dimensions, Vector2i(60, 60));

  // The refused frame must look exactly like the same draw on a renderer that never retained
  // anything, which is what proves the fallback painted the shape.
  RendererTinySkia reference;
  drawOneFrame(reference, 60);
  EXPECT_TRUE(BitmapsEqual(reference.takeSnapshot(), shrunk));
}

TEST(RendererRetainedSpans, InstancedShapeStopsRetaining) {
  SVGDocument document = parseFragment(
      R"svg(<defs><path id="s" d="M 0 0 H 30 V 30 H 0 Z" fill="#a02060"/></defs>
         <use xlink:href="#s" x="5" y="5"/><use xlink:href="#s" x="50" y="50"/>)svg");

  RendererTinySkia renderer;
  renderer.setRetainedSpansEnabled(true);
  renderer.draw(document);
  renderer.draw(document);
  renderer.draw(document);
  const RetainedSpanStats stats = renderer.retainedSpanStats();

  EXPECT_GT(stats.bypassedDraws, 0u)
      << "one entity drawn twice in a frame cannot be described by one retained entry";
  EXPECT_TRUE(BitmapsEqual(renderFresh(document), renderer.takeSnapshot()));
}

TEST(RendererRetainedSpans, PatternPaintIsNotRetained) {
  SVGDocument document = parseFragment(
      R"svg(<defs><pattern id="p" width="12" height="12" patternUnits="userSpaceOnUse">
           <rect width="6" height="6" fill="#c04000"/></pattern></defs>
         <rect x="5" y="5" width="86" height="86" fill="url(#p)"/>)svg");

  RendererTinySkia renderer;
  renderer.setRetainedSpansEnabled(true);
  renderer.draw(document);
  renderer.draw(document);
  const RetainedSpanStats stats = renderer.retainedSpanStats();

  EXPECT_EQ(stats.replayedDraws, 0u)
      << "a pattern's pixels live outside the paint, so its coverage is never replayed";
  EXPECT_TRUE(BitmapsEqual(renderFresh(document), renderer.takeSnapshot()));
}

TEST(RendererRetainedSpans, BudgetEvictsColdestEntries) {
  SVGDocument document = parseFragment(kShapes);

  RendererTinySkia renderer;
  renderer.setRetainedSpansEnabled(true);
  renderer.draw(document);
  const std::size_t settledBytes = renderer.retainedSpanStats().liveBytes;
  ASSERT_GT(settledBytes, 0u);

  // A budget under what one frame needs cannot hold the working set, so the document gives up
  // rather than evict and re-capture the same shapes every frame.
  RendererTinySkia bounded;
  bounded.setRetainedSpansEnabled(true);
  bounded.setRetainedSpanBudgetBytes(settledBytes / 4);
  bounded.draw(document);
  bounded.draw(document);
  const RetainedSpanStats stats = bounded.retainedSpanStats();

  EXPECT_TRUE(stats.documentDisabled)
      << "a working set that does not fit the budget must turn retention off for the document"
      << " (settled " << settledBytes << " bytes, budget " << (settledBytes / 4) << ", live "
      << stats.liveBytes << ", evictions " << stats.evictions << ", captured "
      << stats.capturedDraws << ", replayed " << stats.replayedDraws << ", bypassed "
      << stats.bypassedDraws << ")";
  EXPECT_LE(stats.liveBytes, settledBytes / 4);
  EXPECT_TRUE(BitmapsEqual(renderFresh(document), bounded.takeSnapshot()));
}

TEST(RendererRetainedSpans, BudgetRaiseLetsTheDocumentRetainAgain) {
  SVGDocument document = parseFragment(kShapes);

  RendererTinySkia renderer;
  renderer.setRetainedSpansEnabled(true);
  renderer.setRetainedSpanBudgetBytes(64);
  renderer.draw(document);
  renderer.draw(document);
  ASSERT_TRUE(renderer.retainedSpanStats().documentDisabled);

  renderer.setRetainedSpanBudgetBytes(RetainedSpanDocumentState::kDefaultBudgetBytes);
  renderer.draw(document);
  renderer.draw(document);
  const RetainedSpanStats stats = renderer.retainedSpanStats();

  EXPECT_FALSE(stats.documentDisabled);
  EXPECT_GT(stats.replayedDraws, 0u);
  EXPECT_TRUE(BitmapsEqual(renderFresh(document), renderer.takeSnapshot()));
}

TEST(RendererRetainedSpans, EvictedShapeRendersFromRasterization) {
  // Two documents' worth of shapes in one, with a budget that holds most but not all of them,
  // so eviction runs without the working set being hopeless.
  SVGDocument document = parseFragment(kShapes);

  RendererTinySkia measure;
  measure.setRetainedSpansEnabled(true);
  measure.draw(document);
  const std::size_t settledBytes = measure.retainedSpanStats().liveBytes;
  ASSERT_GT(settledBytes, 0u);

  RendererTinySkia renderer;
  renderer.setRetainedSpansEnabled(true);
  renderer.draw(document);
  // Shrinking the budget after the entries exist evicts the ones not drawn in the frame that
  // notices, which is every entry from an earlier frame.
  renderer.setRetainedSpanBudgetBytes(settledBytes / 2);
  renderer.draw(document);
  const RetainedSpanStats stats = renderer.retainedSpanStats();

  EXPECT_TRUE(stats.evictions > 0u || stats.documentDisabled)
      << "settled " << settledBytes << " bytes, budget " << (settledBytes / 2) << ", live "
      << stats.liveBytes << ", captured " << stats.capturedDraws << ", replayed "
      << stats.replayedDraws << ", bypassed " << stats.bypassedDraws;
  EXPECT_TRUE(BitmapsEqual(renderFresh(document), renderer.takeSnapshot()));
}

TEST(RendererRetainedSpans, AnimatedDocumentStaysCorrectAcrossTimeSamples) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;
  SVGDocument document = instantiateSubtree(
      R"svg(<rect x="0" y="0" width="4" height="16" fill="black">
           <animate attributeName="width" from="4" to="12" begin="0s" dur="2s" fill="freeze"/>
         </rect>)svg",
      options, Vector2i(16, 16));

  RendererTinySkia renderer;
  renderer.setRetainedSpansEnabled(true);

  for (const double time : {0.0, 0.5, 1.0, 1.0, 3.0}) {
    SCOPED_TRACE(time);
    document.setTime(time);
    renderer.draw(document);
    const RendererBitmap retained = renderer.takeSnapshot();

    SVGDocument reference = instantiateSubtree(
        R"svg(<rect x="0" y="0" width="4" height="16" fill="black">
             <animate attributeName="width" from="4" to="12" begin="0s" dur="2s" fill="freeze"/>
           </rect>)svg",
        options, Vector2i(16, 16));
    reference.setTime(time);
    EXPECT_TRUE(BitmapsEqual(renderFresh(reference), retained));
  }
}

}  // namespace donner::svg
