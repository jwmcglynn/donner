/// @file
/// Geode perf-counter assertions.
///
/// These tests are the durable regression signal for the renderer's
/// performance work. They assert `GeodeCounters` ceilings - steady-state
/// buffer / bindgroup / texture / submit / path-encode counts - on
/// representative SVG fixtures.
///
/// Counter ceilings are deterministic; wall-clock budgets are not. That's
/// why these tests (not the benchmark harness) are the CI gate.
///
/// Ceilings track CURRENT observed behaviour with a little slack, so the
/// tests pass today. Each optimization that lands tightens the ceiling(s)
/// it targets; the trailing comment beside each assertion records the
/// steady-state target it drives toward.

#include <gtest/gtest.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Vector2.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"

namespace donner::svg {
namespace {

/// Inline fixture: three disjoint primitives, no gradients/filters/layers.
/// Exercises the pure `submitFillDraw` path - the Tier-1 hot path from
/// design 0030.
constexpr std::string_view kSimpleShapesSvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
  <rect x="10" y="10" width="80" height="80" fill="red"/>
  <circle cx="150" cy="50" r="40" fill="blue"/>
  <ellipse cx="100" cy="150" rx="60" ry="30" fill="green"/>
</svg>
)SVG";

/// Inline fixture: a single defined shape referenced by eight `<use>`
/// instances at distinct positions. The whole document resolves to
/// eight draws of the same source entity - exactly the shape
/// `<use>` instancing targets. Today these draw as
/// eight separate GPU calls; `sameSourceDrawPairs` should report
/// seven (= 8 − 1) adjacent-same-source pairs, which is the draw-call
/// savings an instancing pass would unlock.
constexpr std::string_view kUseHeavySvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     viewBox="0 0 400 100">
  <defs>
    <rect id="r" width="20" height="20" fill="red"/>
  </defs>
  <use xlink:href="#r" x="0"   y="0"/>
  <use xlink:href="#r" x="40"  y="0"/>
  <use xlink:href="#r" x="80"  y="0"/>
  <use xlink:href="#r" x="120" y="0"/>
  <use xlink:href="#r" x="160" y="0"/>
  <use xlink:href="#r" x="200" y="0"/>
  <use xlink:href="#r" x="240" y="0"/>
  <use xlink:href="#r" x="280" y="0"/>
</svg>
)SVG";

/// Inline fixture: a handful of cubic Bezier paths plus one linear
/// gradient. Hits the `fillPathLinearGradient` Tier-1 site and exercises
/// stroke outline encoding on the open-path `path` elements.
constexpr std::string_view kModerateSvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 400">
  <defs>
    <linearGradient id="g1" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="red"/>
      <stop offset="1" stop-color="blue"/>
    </linearGradient>
  </defs>
  <path d="M50,50 C100,0 200,0 250,50 L300,150 Q250,300 150,280 L80,200
           C30,160 20,100 50,50 Z" fill="#336699" opacity="0.8"/>
  <rect x="50" y="300" width="300" height="80" fill="url(#g1)" rx="10"/>
</svg>
)SVG";

/// One sRGB Gaussian blur. Sigma 4 selects six box-blur compute passes.
constexpr std::string_view kFilteredBlurSvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
  <defs>
    <filter id="blur" x="0" y="0" width="100" height="100"
            filterUnits="userSpaceOnUse" color-interpolation-filters="sRGB">
      <feGaussianBlur stdDeviation="4"/>
    </filter>
  </defs>
  <rect x="20" y="20" width="60" height="60" fill="red" filter="url(#blur)"/>
</svg>
)SVG";

/// Dump counters to stderr so the observed values are visible in normal
/// test output (RecordProperty only surfaces in XML). Format keeps each
/// counter on its own column for easy diffing across runs.
void printCounters(const char* label, const geode::GeodeCounters& c) {
  std::fprintf(stderr,
               "[GeodePerf] %-40s  pathEncodes=%4" PRIu64 "  bufferCreates=%5" PRIu64
               "  bindgroupCreates=%5" PRIu64 "  textureCreates=%3" PRIu64 "  submits=%3" PRIu64
               "  drawCalls=%4" PRIu64 "  pipelineSwitches=%3" PRIu64
               "  sameSourceDrawPairs=%3" PRIu64 "  bufferWrites=%5" PRIu64
               "  bufferWriteBytes=%9" PRIu64 "  textureWriteBytes=%9" PRIu64 "\n",
               label, c.pathEncodes, c.bufferCreates, c.bindgroupCreates, c.textureCreates,
               c.submits, c.drawCalls, c.pipelineSwitches, c.sameSourceDrawPairs, c.bufferWrites,
               c.bufferWriteBytes, c.textureWriteBytes);
}

/// Read a file from disk. Returns the empty string on any I/O error -
/// callers treat that as "fixture not available" and skip.
std::string readFile(const std::string& path) {
  std::ifstream f(path);
  if (!f) {
    return {};
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

/// Fully render `svgSource` through a RendererGeode backed by a shared
/// device, then return the per-frame counters.
///
/// `RendererGeode::draw()` internally drives its own `beginFrame` →
/// traversal → `endFrame` cycle using the SVG's own viewBox dimensions,
/// so we just call it directly. An outer `beginFrame` / `endFrame`
/// around `draw()` would be overwritten.
geode::GeodeCounters renderAndGetCounters(std::string_view svgSource,
                                          const std::shared_ptr<geode::GeodeDevice>& device) {
  ParseWarningSink sink = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(svgSource, sink);
  if (parsed.hasError()) {
    ADD_FAILURE() << "ParseSVG failed: " << parsed.error().reason;
    return {};
  }
  SVGDocument document = std::move(parsed.result());

  RendererGeode renderer(device);
  renderer.draw(document);

  // `takeSnapshot()` allocates a readback buffer + issues its own submit.
  // Include it so steady-state cost isn't hidden.
  (void)renderer.takeSnapshot();

  return renderer.lastFrameTimings().counters;
}

class GeodePerfTest : public ::testing::Test {
protected:
  /// Single process-wide device. Each test gets its own `RendererGeode`
  /// backed by this device - matches how production embedders wire things
  /// up (host owns GPU context, many short-lived renderers).
  static std::shared_ptr<geode::GeodeDevice> sharedDevice() {
    static auto device = [] {
      return std::shared_ptr<geode::GeodeDevice>(geode::GeodeDevice::CreateHeadless());
    }();
    return device;
  }
};

// ---------------------------------------------------------------------------
// Fixture: simple shapes (3 solid fills, no gradient, no stroke, no layer).
// ---------------------------------------------------------------------------

TEST_F(GeodePerfTest, SimpleShapes_BaselineCeilings) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  geode::GeodeCounters c = renderAndGetCounters(kSimpleShapesSvg, device);

  // Emit observed values for the next run's ceiling review. Prefer
  // RecordProperty over printf so the numbers survive in the test log
  // without polluting stdout on success.
  RecordProperty("bufferCreates", std::to_string(c.bufferCreates));
  RecordProperty("bindgroupCreates", std::to_string(c.bindgroupCreates));
  RecordProperty("textureCreates", std::to_string(c.textureCreates));
  RecordProperty("submits", std::to_string(c.submits));
  RecordProperty("pathEncodes", std::to_string(c.pathEncodes));
  RecordProperty("drawCalls", std::to_string(c.drawCalls));
  RecordProperty("pipelineSwitches", std::to_string(c.pipelineSwitches));
  RecordProperty("bufferWrites", std::to_string(c.bufferWrites));
  RecordProperty("bufferWriteBytes", std::to_string(c.bufferWriteBytes));
  RecordProperty("textureWriteBytes", std::to_string(c.textureWriteBytes));
  printCounters(::testing::UnitTest::GetInstance()->current_test_info()->name(), c);

  // Observed 2026-04-19 on macOS/Metal, M4 Pro:
  //   baseline:           bufferCreates=13
  //   ssbo+vb arenas:     bufferCreates=7
  //   pooled uniforms:    bufferCreates=5 (4 arenas + 1 readback,
  //                       arenas lazily grown - some frames only
  //                       touch 3 arenas)
  //   device dummies + texture pool: target fresh on first frame;
  //                       repeat-render is 0, see `*_ZeroTextures` below).
  //   draw instrumentation: drawCalls=3 (one per solid fill),
  //                       pipelineSwitches=1 (solid pipeline only).
  EXPECT_LE(c.pathEncodes, 5u);  // Target = 0 on unchanged-geometry frames.
  // Analytic dual-ray fill: each draw now binds 4 extra read-only SSBO
  // arenas (vBands/vCurves/hBandGrid/vBandGrid) on top of bands/curves/vertex/
  // uniform, so first-frame arena growth costs a few more buffer creates.
  EXPECT_LE(c.bufferCreates, 12u);
  EXPECT_LE(c.bindgroupCreates, 6u);  // Target <= #pipelines (3 today).
  EXPECT_LE(c.textureCreates, 3u);    // Target on frame 1; 0 on repeat.
  EXPECT_LE(c.submits, 3u);           // Target = 1.
  EXPECT_LE(c.drawCalls, 4u);         // 3 shapes, one draw each.
  EXPECT_LE(c.pipelineSwitches, 2u);  // Solid pipeline bound once.
}

// ---------------------------------------------------------------------------
// Fixture: one gradient path + one rounded-rect filled with the gradient.
// ---------------------------------------------------------------------------

TEST_F(GeodePerfTest, Moderate_BaselineCeilings) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  geode::GeodeCounters c = renderAndGetCounters(kModerateSvg, device);

  RecordProperty("bufferCreates", std::to_string(c.bufferCreates));
  RecordProperty("bindgroupCreates", std::to_string(c.bindgroupCreates));
  RecordProperty("textureCreates", std::to_string(c.textureCreates));
  RecordProperty("submits", std::to_string(c.submits));
  RecordProperty("pathEncodes", std::to_string(c.pathEncodes));
  RecordProperty("drawCalls", std::to_string(c.drawCalls));
  RecordProperty("pipelineSwitches", std::to_string(c.pipelineSwitches));
  RecordProperty("bufferWrites", std::to_string(c.bufferWrites));
  RecordProperty("bufferWriteBytes", std::to_string(c.bufferWriteBytes));
  RecordProperty("textureWriteBytes", std::to_string(c.textureWriteBytes));
  printCounters(::testing::UnitTest::GetInstance()->current_test_info()->name(), c);

  // Observed 2026-04-19:
  //   baseline:       bufferCreates=10 submits=4 textureCreates=10
  //   ssbo+vb arenas: bufferCreates=10 submits=4 textureCreates=10
  //                   (three encoders from push/pop each allocate
  //                   their own arenas - 3×3 + 1 readback.)
  //   shared encoder: bufferCreates=10 submits=2 textureCreates=10
  //                   (push/pop no longer forces a queue submit)
  //   device dummies + texture pool: target + layer on frame 1, 0 on repeat.
  //   draw instrumentation: drawCalls=2 (solid fill + gradient fill),
  //                   pipelineSwitches=~3 (solid for layer, image
  //                   blit on layer composite, gradient for rect).
  EXPECT_LE(c.pathEncodes, 4u);  // Target = 0.
  // Analytic dual-ray: fill + gradient each grow 4 extra SSBO arenas
  // (vBands/vCurves/hBandGrid/vBandGrid) on first use.
  EXPECT_LE(c.bufferCreates, 20u);
  EXPECT_LE(c.bindgroupCreates, 6u);  // Target <= #pipelines.
  EXPECT_LE(c.textureCreates, 6u);    // Target + layer on first render; 0 on repeat.
  EXPECT_LE(c.submits, 3u);           // Target = 2 steady-state (frame + readback).
  EXPECT_LE(c.drawCalls, 6u);         // 2 fills + blit composites.
  EXPECT_LE(c.pipelineSwitches, 6u);  // Solid / gradient / image pipelines + mask if any.
}

/// One rect with a huge-radius feMorphology (erode). The morphology filter
/// decomposes the radius into per-axis passes capped at 31 device pixels, so
/// a 9,999-user-pixel radius produces hundreds of compute passes on a
/// viewBox-sized canvas. The shared filter encoder must chunk those passes
/// into bounded command buffers instead of recording one unbounded buffer.
constexpr std::string_view kHugeRadiusMorphologySvg = R"SVG(
<svg viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">
  <filter id="filter1">
    <feMorphology radius="9999"/>
  </filter>
  <rect x="20" y="20" width="160" height="160" fill="red" filter="url(#filter1)"/>
</svg>
)SVG";

// ---------------------------------------------------------------------------
// Fixture: image blits - drawImage uniform pooling.
// ---------------------------------------------------------------------------

TEST_F(GeodePerfTest, MultiImageBlit_PoolsCompositeUniforms) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  // Three `drawImage` blits in one frame. Each blit previously created its
  // own 160-byte uniform buffer; pooled blits should bump-allocate from the
  // encoder's per-frame uniform scratch instead, so the only buffer create
  // left is the scratch arena's first growth.
  RendererGeode renderer(device);
  RenderViewport viewport;
  viewport.size = Vector2d(80.0, 32.0);
  viewport.devicePixelRatio = 1.0;
  renderer.beginFrame(viewport);

  ImageResource image;
  image.width = 2;
  image.height = 2;
  image.data = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};

  ImageParams params;
  params.targetRect = Box2d({8.0, 8.0}, {24.0, 24.0});
  params.opacity = 1.0;

  renderer.drawImage(image, params);
  params.targetRect = Box2d({32.0, 8.0}, {48.0, 24.0});
  renderer.drawImage(image, params);
  params.targetRect = Box2d({56.0, 8.0}, {72.0, 24.0});
  renderer.drawImage(image, params);
  renderer.endFrame();

  const geode::GeodeCounters c = renderer.lastFrameTimings().counters;
  RecordProperty("bufferCreates", std::to_string(c.bufferCreates));
  printCounters(::testing::UnitTest::GetInstance()->current_test_info()->name(), c);

  EXPECT_LE(c.bufferCreates, 1u)
      << "Three image blits should pool their uniform uploads into the encoder scratch arena "
         "(one create for the arena's first growth) instead of creating one uniform buffer per "
         "blit.";
}

TEST_F(GeodePerfTest, MorphologyHugeRadius_ChunksFilterCommandBuffers) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  geode::GeodeCounters c = renderAndGetCounters(kHugeRadiusMorphologySvg, device);

  RecordProperty("submits", std::to_string(c.submits));
  printCounters(::testing::UnitTest::GetInstance()->current_test_info()->name(), c);

  // The shared-encoder design keeps a small blur at two submissions (frame
  // + readback). A pathological morphology (323 X + 323 Y passes here) must
  // not regress that design into one unbounded command buffer: every 64
  // passes the filter arena submits its chunk and starts a fresh encoder,
  // so 646 passes produce 10 chunk submits plus the final frame submit and
  // the readback submit, 12 in total. Assert a bounded range rather than
  // the exact count so incidental extra submits (device warm-up, snapshot
  // plumbing) don't turn this into a change-detector. The pass count
  // scales with the DEVICE-pixel radius, so this ceiling is only valid at
  // this fixture's fixed 200x200 canvas at devicePixelRatio 1; rescaling
  // the fixture requires retuning both bounds.
  EXPECT_GE(c.submits, 6u)
      << "Huge-radius morphology should force chunked filter submissions beyond the shared "
         "frame + readback pair.";
  EXPECT_LE(c.submits, 20u)
      << "Chunked filter submissions should bound the command buffer to 64 passes each: "
         "646 passes chunk into 10 mid-frame submits plus frame and readback submits.";
}

TEST_F(GeodePerfTest, GaussianBlur_UsesSingleFrameSubmission) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const geode::GeodeCounters c = renderAndGetCounters(kFilteredBlurSvg, device);

  RecordProperty("submits", std::to_string(c.submits));
  RecordProperty("bufferCreates", std::to_string(c.bufferCreates));
  RecordProperty("textureCreates", std::to_string(c.textureCreates));
  printCounters(::testing::UnitTest::GetInstance()->current_test_info()->name(), c);

  EXPECT_LE(c.submits, 2u)
      << "The filter layer, all Gaussian passes, composite, and readback should require only the "
         "shared frame submission plus the snapshot submission.";
}


// ---------------------------------------------------------------------------
// Fixture: lion.svg - the workhorse SVG used across Donner test suites.
// Skipped gracefully if the file is not bundled (e.g. unit test run without
// testdata deps).
// ---------------------------------------------------------------------------

TEST_F(GeodePerfTest, Lion_BaselineCeilings) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const std::string svg = readFile("donner/svg/renderer/testdata/lion.svg");
  if (svg.empty()) {
    GTEST_SKIP() << "testdata/lion.svg not readable - ensure the test target "
                 << "has testdata as a data dep.";
    return;
  }

  geode::GeodeCounters c = renderAndGetCounters(svg, device);

  RecordProperty("bufferCreates", std::to_string(c.bufferCreates));
  RecordProperty("bindgroupCreates", std::to_string(c.bindgroupCreates));
  RecordProperty("textureCreates", std::to_string(c.textureCreates));
  RecordProperty("submits", std::to_string(c.submits));
  RecordProperty("pathEncodes", std::to_string(c.pathEncodes));
  RecordProperty("drawCalls", std::to_string(c.drawCalls));
  RecordProperty("pipelineSwitches", std::to_string(c.pipelineSwitches));
  RecordProperty("bufferWrites", std::to_string(c.bufferWrites));
  RecordProperty("bufferWriteBytes", std::to_string(c.bufferWriteBytes));
  RecordProperty("textureWriteBytes", std::to_string(c.textureWriteBytes));
  printCounters(::testing::UnitTest::GetInstance()->current_test_info()->name(), c);

  // Observed 2026-04-19:
  //   baseline:           bufferCreates=529 (132 paths × 4 + 1 readback)
  //   ssbo+vb arenas:     bufferCreates=137 (3 arenas + 132 uniforms
  //                       + dummies + readback; 74% drop)
  //   pooled uniforms:    bufferCreates=6 (4 arenas + 2 dummies +
  //                       readback; 98.9% total drop from baseline)
  //   bindgroupCreates=132 (one per draw; a shared bind group
  //                       collapses this to ~1).
  //   device dummies + texture pool: target on frame 1; 0 on repeat.
  //   draw instrumentation: drawCalls=132 (one per path),
  //                       pipelineSwitches=1 (solid pipeline only -
  //                       all of Lion is solid-fill). `<use>` instancing
  //                       is the knob that moves drawCalls for
  //                       `<use>`-heavy fixtures; Lion has no `<use>`
  //                       so this ceiling stays at 132.
  EXPECT_LE(c.pathEncodes, 200u);  // Target = 0.
  // GPU residence: frame 1 now front-loads ONE persistent
  // combined buffer per cached solid-fill path (132 for Lion) so that
  // steady-state frames re-upload zero geometry. This trades a one-time
  // frame-1 buffer-create spike for the steady-state win asserted in
  // `Lion_NoDirtyPath_ZeroEncodes` (frame-2 bufferCreates == 1, the
  // readback). Ceiling = 132 resident + readback + arena slack.
  EXPECT_LE(c.bufferCreates, 150u);
  EXPECT_LE(c.bindgroupCreates, 200u);   // Target <= #pipelines.
  EXPECT_LE(c.textureCreates, 3u);       // Target on first render; 0 on repeat.
  EXPECT_LE(c.submits, 3u);              // Target = 1.
  EXPECT_LE(c.drawCalls, 200u);          // 132 paths, one draw each (no <use>).
  EXPECT_LE(c.pipelineSwitches, 2u);     // All-solid fixture: tracker binds solid once.
  EXPECT_EQ(c.sameSourceDrawPairs, 0u);  // No `<use>` in Lion - every draw has a unique source.
}

// ---------------------------------------------------------------------------
// Fixture: eight `<use>` instances of one source `<rect>`. Motivates
// `<use>` instancing: today these render as eight
// separate GPU draws; the future batcher collapses them into one
// instanced draw. The detection counter `sameSourceDrawPairs` reports
// seven adjacent-same-source pairs = the draw-call savings an
// instancing pass would unlock.
// ---------------------------------------------------------------------------

TEST_F(GeodePerfTest, UseHeavy_BaselineCeilings) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  geode::GeodeCounters c = renderAndGetCounters(kUseHeavySvg, device);

  RecordProperty("bufferCreates", std::to_string(c.bufferCreates));
  RecordProperty("bindgroupCreates", std::to_string(c.bindgroupCreates));
  RecordProperty("textureCreates", std::to_string(c.textureCreates));
  RecordProperty("submits", std::to_string(c.submits));
  RecordProperty("pathEncodes", std::to_string(c.pathEncodes));
  RecordProperty("drawCalls", std::to_string(c.drawCalls));
  RecordProperty("pipelineSwitches", std::to_string(c.pipelineSwitches));
  RecordProperty("bufferWrites", std::to_string(c.bufferWrites));
  RecordProperty("bufferWriteBytes", std::to_string(c.bufferWriteBytes));
  RecordProperty("textureWriteBytes", std::to_string(c.textureWriteBytes));
  RecordProperty("sameSourceDrawPairs", std::to_string(c.sameSourceDrawPairs));
  printCounters(::testing::UnitTest::GetInstance()->current_test_info()->name(), c);

  // The `<use>` instances share one source `<rect>` entity, so
  // its encoded path is cached once and reused for all eight draws
  // on frame 1 (first draw encodes, next seven hit the cache).
  EXPECT_LE(c.pathEncodes, 2u);
  // The batcher collapses all eight consecutive
  // same-source `<use>` draws into a single instanced GPU call.
  EXPECT_EQ(c.drawCalls, 1u);
  // And a single bind group covers all eight - per-instance
  // transforms ride in a storage-buffer binding (binding 7), while
  // the other seven entries are stable across the batch. The second
  // bind group is the GPU-side snapshot readback (one per snapshot;
  // the input texture view changes per render target, so it cannot
  // be pooled like the staging texture and readback buffer).
  EXPECT_EQ(c.bindgroupCreates, 2u);
  // Seven adjacent-same-source pairs detected at `drawPath` entry
  // (BEFORE batching collapses them). This is the "opportunity"
  // counter - kept as a separate signal from `drawCalls` (the
  // "realized" counter) so a regression in the detection path
  // (e.g. `<use>` stops resolving to a shared `dataEntity`) or
  // in the batcher trips independently.
  EXPECT_EQ(c.sameSourceDrawPairs, 7u);
}

// ---------------------------------------------------------------------------
// Double-render sanity: same renderer, two consecutive frames. Counters
// should reflect only the SECOND frame (beginFrame resets). This guards
// the reset path - if a future optimization accidentally persists state
// across frames, this test catches it.
// ---------------------------------------------------------------------------

TEST_F(GeodePerfTest, CountersResetBetweenFrames) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  ParseWarningSink sink = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(kSimpleShapesSvg, sink);
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());

  RendererGeode renderer(device);

  // First frame. `draw()` internally manages beginFrame/endFrame using
  // the document's viewBox dimensions (200×200 for kSimpleShapesSvg).
  renderer.draw(document);
  (void)renderer.takeSnapshot();

  const auto firstCounters = renderer.lastFrameTimings().counters;
  EXPECT_GT(firstCounters.pathEncodes, 0u);  // Sanity: something happened.

  // Second frame: same document, same size. Counters reset in beginFrame
  // and should accumulate only this frame's work. Render targets are
  // reused across same-size frames, so the second
  // frame's textureCreates should be strictly smaller.
  renderer.draw(document);
  (void)renderer.takeSnapshot();

  const auto secondCounters = renderer.lastFrameTimings().counters;

  // Second-frame counters are strictly this-frame only (beginFrame
  // resets). With `GeodePathCacheComponent` in place, an unchanged
  // second render does zero path encodes - the explicit assertion on
  // that invariant lives in `SimpleShapes_NoDirtyPath_ZeroEncodes`;
  // here we just confirm the reset path preserves the invariant across
  // counters and that render targets get reused.
  EXPECT_EQ(secondCounters.pathEncodes, 0u);
  EXPECT_LE(secondCounters.pathEncodes, firstCounters.pathEncodes);
  EXPECT_LT(secondCounters.textureCreates, firstCounters.textureCreates)
      << "Second frame should create STRICTLY FEWER textures than the first "
      << "because render targets are reused at the same size.";
}

// ---------------------------------------------------------------------------
// GeodePathCacheComponent: no-geometry-change ⇒ zero encodes.
//
// The promise: once a document has been rendered once, rendering it again
// with no geometry or style mutation must perform zero CPU-side path
// encodes. `GeodePathCacheComponent` holds the `EncodedPath` across frames;
// `ComputedPathComponent` equality gating in `ShapeSystem` means its entt
// `on_update` signal only fires when geometry actually changes, so the
// cache survives idle re-renders.
//
// These tests use one `RendererGeode` across two `draw()` calls so the
// cache component on the document's registry is live on frame 2.
// ---------------------------------------------------------------------------

/// Helper: two consecutive renders of the same document, returning only
/// the SECOND frame's counters. Used by the zero-encode assertions.
geode::GeodeCounters countersForSecondRender(std::string_view svgSource,
                                             const std::shared_ptr<geode::GeodeDevice>& device) {
  ParseWarningSink sink = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(svgSource, sink);
  if (parsed.hasError()) {
    ADD_FAILURE() << "ParseSVG failed: " << parsed.error().reason;
    return {};
  }
  SVGDocument document = std::move(parsed.result());

  RendererGeode renderer(device);
  renderer.draw(document);
  (void)renderer.takeSnapshot();
  // First-frame counters intentionally discarded - we only care about the
  // steady-state second frame.

  renderer.draw(document);
  (void)renderer.takeSnapshot();
  return renderer.lastFrameTimings().counters;
}

TEST_F(GeodePerfTest, SimpleShapes_NoDirtyPath_ZeroEncodes) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const geode::GeodeCounters c = countersForSecondRender(kSimpleShapesSvg, device);
  printCounters("SimpleShapes_NoDirtyPath_ZeroEncodes (frame2)", c);

  // Target: second frame does zero path encodes - three shapes hit the
  // cache. countPathEncode() is only called on cache miss.
  EXPECT_EQ(c.pathEncodes, 0u) << "Cache miss on an unchanged second render: one or more paths "
                                  "re-encoded despite zero geometry changes.";
  // `GeodeBufferPool`: steady-state arena buffers are recycled across
  // frames. The remaining create is the takeSnapshot readback buffer.
  EXPECT_LE(c.bufferCreates, 2u) << "Arena buffer churn on an unchanged second render: the "
                                    "cross-frame GeodeBufferPool should serve all arena growth.";
  // GPU residence steady-state: the three solid fills are
  // GPU-resident from frame 1, so frame 2 writes zero geometry AND zero
  // uniform bytes (same viewport => byte-identical uniforms) and reuses
  // its cached bind groups. The pre-residence baseline was bufferWriteBytes=4128,
  // bindgroupCreates=3.
  EXPECT_LE(c.bufferWriteBytes, 512u)
      << "Steady-state buffer writes on an unchanged second render: resident geometry "
         "should not re-upload.";
  EXPECT_LE(c.bindgroupCreates, 1u)
      << "Steady-state bind-group creates: resident fills should reuse cached bind groups.";
}

TEST_F(GeodePerfTest, Moderate_NoDirtyPath_ZeroEncodes) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const geode::GeodeCounters c = countersForSecondRender(kModerateSvg, device);
  printCounters("Moderate_NoDirtyPath_ZeroEncodes (frame2)", c);

  // Target: zero encodes across both fill paths - confirms the cache
  // covers both `submitFillDraw` (opacity-layer path) and
  // `fillPathLinearGradient` (rounded-rect path).
  EXPECT_EQ(c.pathEncodes, 0u) << "Cache miss on unchanged second render: fill or gradient path "
                                  "re-encoded despite zero geometry changes.";
  // `GeodeBufferPool`: readback + one per-blit uniform buffer remain
  // (layer composite blits create a fresh uniform buffer per call).
  EXPECT_LE(c.bufferCreates, 4u) << "Arena buffer churn on an unchanged second render.";
  // GPU residence: the opacity-layer solid path is resident, so
  // the steady-state residual is only the gradient rect (arena path) plus
  // the isolated-layer composite blit's per-frame uniform. The pre-residence
  // baseline was bufferWriteBytes=5372, bindgroupCreates=3.
  EXPECT_LE(c.bufferWriteBytes, 3200u)
      << "Steady-state buffer writes: only the gradient + layer-blit uniforms should remain.";
  EXPECT_LE(c.bindgroupCreates, 3u) << "Steady-state bind-group creates: the resident solid fill "
                                       "should reuse its cached bind group.";
}

TEST_F(GeodePerfTest, Lion_NoDirtyPath_ZeroEncodes) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const std::string svg = readFile("donner/svg/renderer/testdata/lion.svg");
  if (svg.empty()) {
    GTEST_SKIP() << "testdata/lion.svg not readable - ensure the test target "
                 << "has testdata as a data dep.";
    return;
  }

  const geode::GeodeCounters c = countersForSecondRender(svg, device);
  printCounters("Lion_NoDirtyPath_ZeroEncodes (frame2)", c);

  // Target: 132 cached paths → zero re-encodes. This is the headline
  // assertion: the lion is our standard "many paths" stress fixture and
  // the pre-cache baseline showed 132 pathEncodes per frame. Driving that to 0
  // is the whole point of the cache.
  EXPECT_EQ(c.pathEncodes, 0u) << "Cache miss on unchanged second render of lion.svg: "
                                  "re-encoded paths despite zero geometry changes.";
  // `GeodeBufferPool`: pre-pool this was 12 creates/frame (arena
  // re-growth in the per-frame encoder); pooled steady state is the
  // readback buffer only.
  EXPECT_LE(c.bufferCreates, 3u) << "Arena buffer churn on an unchanged second render of lion.svg.";
  // GPU residence - the headline steady-state win. All 132 solid
  // fills are GPU-resident from frame 1, so an unchanged second render
  // re-uploads zero geometry bytes and creates zero bind groups. The
  // pre-residence baseline was bufferWriteBytes=172776 across 1056 writes, and
  // bindgroupCreates=132 (one per draw). drawCalls stays 132 - the draws
  // still happen, they just bind cached resident buffers.
  EXPECT_LE(c.bufferWriteBytes, 4096u)
      << "Steady-state geometry re-upload on an unchanged second render of lion.svg: resident "
         "buffers should serve every draw.";
  EXPECT_LE(c.bindgroupCreates, 2u)
      << "Steady-state bind-group creates no longer proportional to draw calls (was 132).";
}

TEST_F(GeodePerfTest, GhostscriptTiger_NoDirtyPath_ZeroEncodes) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const std::string svg = readFile("donner/svg/renderer/testdata/Ghostscript_Tiger.svg");
  if (svg.empty()) {
    GTEST_SKIP() << "testdata/Ghostscript_Tiger.svg not readable - ensure the "
                 << "test target has testdata as a data dep.";
    return;
  }

  const geode::GeodeCounters c = countersForSecondRender(svg, device);
  printCounters("GhostscriptTiger_NoDirtyPath_ZeroEncodes (frame2)", c);

  // Target: Tiger has strokes too, so this test also exercises the
  // stroke slot of `GeodePathCacheComponent` - a second render must not
  // re-run `Path::strokeToFill` nor re-encode the stroked outline.
  EXPECT_EQ(c.pathEncodes, 0u)
      << "Cache miss on unchanged second render of Ghostscript_Tiger.svg: "
         "fill- or stroke-slot cache missed despite zero geometry changes.";
  // `GeodeBufferPool`: pre-pool this was 20 creates/frame. Observed
  // pooled steady state is 4 (readback + residual arena churn); assert
  // with a little margin.
  EXPECT_LE(c.bufferCreates, 8u)
      << "Arena buffer churn on an unchanged second render of Ghostscript_Tiger.svg.";
  // GPU residence - THE headline acceptance target. The pre-residence
  // measured profile was ~1.44 MB re-uploaded
  // per steady-state frame across 2,432 writeBuffer calls, plus 304 bind
  // group creates (one per draw). With persistent per-entity residence an
  // unchanged second render re-uploads zero geometry bytes and creates
  // zero bind groups - a >99% reduction, far past the 90% acceptance
  // floor (which would be <= 147388 bytes). Both fill AND stroke slots
  // are exercised (Tiger has strokes). drawCalls stays 304.
  EXPECT_LE(c.bufferWriteBytes, 8192u)
      << "Steady-state geometry re-upload on an unchanged second render of Ghostscript_Tiger.svg. "
         "Wave-1 baseline was 1,473,888 bytes; residency should drive this to ~0.";
  EXPECT_LE(c.bindgroupCreates, 2u)
      << "Steady-state bind-group creates no longer proportional to draw calls (was 304).";
}

// ---------------------------------------------------------------------------
// Transient render-target pool - zero-alloc repeat render.
//
// A size-keyed free list for layer / filter / mask / clip-mask scratch
// textures. Combined with same-size render-target reuse and the
// device-shared persistent dummy texture, a repeat render of the same
// document at the same size should allocate zero textures on frame 2.
// ---------------------------------------------------------------------------

TEST_F(GeodePerfTest, SimpleShapes_NoDirtyPath_ZeroTextures) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const geode::GeodeCounters c = countersForSecondRender(kSimpleShapesSvg, device);
  printCounters("SimpleShapes_NoDirtyPath_ZeroTextures (frame2)", c);

  // Exercises only the main target; no isolated layers.
  // Any texture allocation here means render-target reuse regressed or a dummy is
  // leaking per-frame.
  EXPECT_EQ(c.textureCreates, 0u)
      << "Texture allocation on unchanged second render: main target or "
         "dummy texture leaking across frames.";
}

TEST_F(GeodePerfTest, Moderate_NoDirtyPath_ZeroTextures) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const geode::GeodeCounters c = countersForSecondRender(kModerateSvg, device);
  printCounters("Moderate_NoDirtyPath_ZeroTextures (frame2)", c);

  // Moderate fixture has `<path opacity="0.8">` which triggers a
  // `pushIsolatedLayer` / `popIsolatedLayer` round-trip per frame. The
  // layer allocates an RGBA8 target that the texture pool must reuse.
  EXPECT_EQ(c.textureCreates, 0u) << "Isolated-layer texture leak on unchanged second render. "
                                     "Layer push/pop should draw from the M4.2 texture pool.";
}

TEST_F(GeodePerfTest, GaussianBlur_NoDirtyPath_ZeroTextures) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const geode::GeodeCounters c = countersForSecondRender(kFilteredBlurSvg, device);
  printCounters("GaussianBlur_NoDirtyPath_ZeroTextures (frame2)", c);

  EXPECT_EQ(c.textureCreates, 0u)
      << "Filter intermediate textures should return to the device-shared texture pool after the "
         "frame submits.";
}

/// One rect with a linear-gradient stroke. The stroked outline is cached
/// by the M2 stroke cache, so an unchanged frame should re-upload zero
/// geometry once gradient strokes are resident.
constexpr std::string_view kGradientStrokeSvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
  <defs>
    <linearGradient id="g" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="red"/>
      <stop offset="1" stop-color="blue"/>
    </linearGradient>
  </defs>
  <rect x="20" y="20" width="60" height="60" fill="none"
        stroke="url(#g)" stroke-width="6"/>
</svg>
)SVG";

TEST_F(GeodePerfTest, GradientStroke_NoDirtyPath_ZeroWrites) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  geode::GeodeCounters c = countersForSecondRender(kGradientStrokeSvg, device);
  RecordProperty("bufferWrites", std::to_string(c.bufferWrites));
  RecordProperty("bufferWriteBytes", std::to_string(c.bufferWriteBytes));
  RecordProperty("bindgroupCreates", std::to_string(c.bindgroupCreates));
  printCounters(::testing::UnitTest::GetInstance()->current_test_info()->name(), c);

  EXPECT_EQ(c.pathEncodes, 0u) << "Stroke outline must come from the M2 stroke cache.";
  EXPECT_EQ(c.bufferWriteBytes, 0u)
      << "Resident gradient strokes must re-upload zero geometry on an unchanged frame.";
}

TEST_F(GeodePerfTest, Lion_NoDirtyPath_ZeroTextures) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const std::string svg = readFile("donner/svg/renderer/testdata/lion.svg");
  if (svg.empty()) {
    GTEST_SKIP() << "testdata/lion.svg not readable.";
    return;
  }

  const geode::GeodeCounters c = countersForSecondRender(svg, device);
  printCounters("Lion_NoDirtyPath_ZeroTextures (frame2)", c);

  EXPECT_EQ(c.textureCreates, 0u) << "Texture allocation on unchanged second render of lion.svg.";
}

TEST_F(GeodePerfTest, GhostscriptTiger_NoDirtyPath_ZeroTextures) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const std::string svg = readFile("donner/svg/renderer/testdata/Ghostscript_Tiger.svg");
  if (svg.empty()) {
    GTEST_SKIP() << "testdata/Ghostscript_Tiger.svg not readable.";
    return;
  }

  const geode::GeodeCounters c = countersForSecondRender(svg, device);
  printCounters("GhostscriptTiger_NoDirtyPath_ZeroTextures (frame2)", c);

  EXPECT_EQ(c.textureCreates, 0u)
      << "Texture allocation on unchanged second render of Ghostscript_Tiger.svg.";
}

// ---------------------------------------------------------------------------
// Issue #575 regression: constructing many `RendererGeode` instances on a
// shared `GeodeDevice` must not leak pipeline allocations.
//
// The original fix (moving pipeline ownership onto `GeodeDevice`) was
// worth ~1.6 MB per renderer across the resvg test suite. This test
// pins the contract: a fresh renderer may allocate a handful of bucket-
// sized resources (the target goes into the per-instance pool,
// plus a readback buffer for `takeSnapshot`), but every *pipeline*
// lives on the device. Doubling the renderer count therefore roughly
// doubles textures/buffers, not pipelines - and the per-renderer
// overhead stays comfortably under the ceiling we assert below.
// ---------------------------------------------------------------------------
TEST_F(GeodePerfTest, SharedDevice_RendererConstructionDoesNotLeakPipelines) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  // Warm the lazy caches on the shared device (mask pipeline, any
  // first-render device-side buffers) so we measure steady-state
  // per-renderer overhead only.
  (void)renderAndGetCounters(kSimpleShapesSvg, device);

  const uint64_t baselineTex = device->lifetimeTextureCreates();
  const uint64_t baselineBuf = device->lifetimeBufferCreates();

  // 50 renderers is enough to trip the old pipeline-leak path by roughly
  // an order of magnitude (900 pipelines × 50 = 45k wgpu pipeline objects
  // under the old code). 50 is also small enough to keep the test fast.
  constexpr int kRendererCount = 50;
  for (int i = 0; i < kRendererCount; ++i) {
    (void)renderAndGetCounters(kSimpleShapesSvg, device);
  }

  const uint64_t deltaTex = device->lifetimeTextureCreates() - baselineTex;
  const uint64_t deltaBuf = device->lifetimeBufferCreates() - baselineBuf;

  // `renderAndGetCounters` constructs one renderer, renders one frame,
  // and takes a snapshot. Per iteration that legitimately creates:
  //   - one render target
  //   - a readback buffer inside `takeSnapshot`
  //   - ~4 arena buffers for the vertex/band/curve/uniform data of
  //     the three solid fills in `kSimpleShapesSvg`
  // Budget liberally: 4 textures and 8 buffers per iteration absorbs
  // a little slack. Anything larger than that means
  // something new is being allocated per-renderer - most likely a
  // pipeline, which is exactly what this test guards against.
  EXPECT_LE(deltaTex, 4u * kRendererCount) << "Per-renderer texture growth exceeds expected "
                                              "(target + slack). Did pipeline construction "
                                              "move back onto `RendererGeode::Impl`?";
  // 0041 analytic dual-ray fill adds 4 read-only SSBO arenas per encoder
  // (vBands/vCurves/hBandGrid/vBandGrid), so per-renderer first-frame arena
  // growth is a few buffers higher than the pre-analytic 3-arena layout.
  EXPECT_LE(deltaBuf, 11u * kRendererCount) << "Per-renderer buffer growth exceeds expected "
                                               "(arenas + readback + slack). See issue #575.";
}

TEST_F(GeodePerfTest, TextureSnapshotStressReleasesTargets) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  ParseWarningSink sink = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(kSimpleShapesSvg, sink);
  ASSERT_FALSE(parsed.hasError()) << parsed.error().reason;
  SVGDocument document = std::move(parsed.result());

  RendererGeode renderer(device);
  const uint64_t textureReleasesBefore =
      geode::ScopedWgpuHandle<wgpu::Texture>::releaseCountForTesting();

  constexpr int kFrameCount = 12;
  for (int frame = 0; frame < kFrameCount; ++frame) {
    renderer.draw(document);
    std::shared_ptr<const RendererTextureSnapshot> snapshot = renderer.takeTextureSnapshot();
    ASSERT_NE(snapshot, nullptr);
    snapshot.reset();
    device->drainDeferredDestroys();

    const uint64_t textureReleaseDelta =
        geode::ScopedWgpuHandle<wgpu::Texture>::releaseCountForTesting() - textureReleasesBefore;
    EXPECT_GE(textureReleaseDelta, static_cast<uint64_t>(frame + 1))
        << "Texture snapshot frame " << frame
        << " did not release its transferred single-sample target.";
  }
}

// ---------------------------------------------------------------------------
// GPU residence: eviction / no-unbounded-growth.
//
// Persistent per-entity GPU buffers must not accumulate across distinct
// documents. Each document owns its own ECS registry; when the document is
// torn down its `GeodeResidentPathComponent`s are destroyed and their GPU
// buffers freed (RAII, settling the device's live-resident-bytes gauge).
// This test renders a series of distinct documents on the shared device and
// asserts the live resident bytes return to baseline after each teardown -
// i.e. GPU memory is bounded no matter how many documents pass through.
// ---------------------------------------------------------------------------

TEST_F(GeodePerfTest, GpuResidence_FreesResidentBuffersOnDocumentTeardown) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  // Baseline: any residence from earlier tests has already been freed
  // (their renderers + documents were destroyed), so this is the floor we
  // must return to after every document below.
  const int64_t baseline = device->liveResidentBytesForTesting();

  int64_t peakWhileLive = baseline;
  for (int i = 0; i < 8; ++i) {
    // Distinct documents (varying shape count) => distinct registries and
    // distinct resident buffers.
    std::ostringstream svg;
    svg << R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 400">)";
    const int shapes = 3 + i;
    for (int s = 0; s < shapes; ++s) {
      svg << "<rect x=\"" << (10 + s * 5) << "\" y=\"" << (10 + s * 7) << "\" width=\"40\" "
          << "height=\"30\" fill=\"rgb(" << (s * 20 % 256) << ",64,128)\"/>";
    }
    svg << "</svg>";

    ParseWarningSink sink = ParseWarningSink::Disabled();
    auto parsed = parser::SVGParser::ParseSVG(svg.str(), sink);
    ASSERT_FALSE(parsed.hasError()) << parsed.error().reason;

    {
      SVGDocument document = std::move(parsed.result());
      RendererGeode renderer(device);
      renderer.draw(document);
      (void)renderer.takeSnapshot();
      peakWhileLive = std::max(peakWhileLive, device->liveResidentBytesForTesting());
    }  // document + renderer destroyed => resident components freed.

    EXPECT_EQ(device->liveResidentBytesForTesting(), baseline)
        << "Resident GPU buffers from document " << i
        << " were not freed on teardown - residence is leaking across documents.";
  }

  EXPECT_GT(peakWhileLive, baseline)
      << "Sanity: at least one document should have held live resident GPU buffers.";
  EXPECT_EQ(device->liveResidentBytesForTesting(), baseline)
      << "Live resident bytes must return to baseline after all documents are torn down.";
}

// ---------------------------------------------------------------------------
// GPU residence: repeated same-document renders hold residence
// steady - they must not grow the live resident-bytes gauge frame over
// frame (the buffers are reused, not reallocated).
// ---------------------------------------------------------------------------

TEST_F(GeodePerfTest, GpuResidence_SteadyAcrossRepeatedRenders) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const int64_t baseline = device->liveResidentBytesForTesting();

  ParseWarningSink sink = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(kSimpleShapesSvg, sink);
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());

  RendererGeode renderer(device);
  renderer.draw(document);
  (void)renderer.takeSnapshot();
  const int64_t afterFirst = device->liveResidentBytesForTesting();
  EXPECT_GT(afterFirst, baseline) << "First render should establish GPU residence.";

  for (int i = 0; i < 6; ++i) {
    renderer.draw(document);
    (void)renderer.takeSnapshot();
    EXPECT_EQ(device->liveResidentBytesForTesting(), afterFirst)
        << "Repeated unchanged render must not grow resident GPU memory (frame " << i << ").";
  }
}

// ---------------------------------------------------------------------------
// GPU residence: cross-device safety.
//
// A `GeodeResidentPathComponent` lives on the SVGDocument's ECS registry, so
// it survives the `RendererGeode` that filled it. If the SAME document is
// later rendered by a SECOND `RendererGeode` backed by a DIFFERENT
// `GeodeDevice`, the resident buffer + bind group created by the first device
// must NOT be bound into the second device's render pass - WebGPU rejects
// cross-device resources. The fix scopes residence to the owning device
// (`GeodeDevice::deviceId()`) and forces a re-upload when the device changes.
//
// Regression: on the pre-fix branch tip device B binds device A's stale
// handles, so its output diverges from device A's (dropped draw / validation
// error). This test renders one document on two independent devices and
// asserts bit-identical output; it FAILS on the pre-fix tip and PASSES with
// the fix.
// ---------------------------------------------------------------------------

// Identical dimensions and identical visible pixels (each bitmap's own
// `rowBytes` absorbs any inter-row padding difference).
bool bitmapsEqual(const RendererBitmap& a, const RendererBitmap& b) {
  if (a.dimensions != b.dimensions) {
    return false;
  }
  const int w = a.dimensions.x;
  const int h = a.dimensions.y;
  for (int y = 0; y < h; ++y) {
    const uint8_t* ra = a.pixels.data() + static_cast<size_t>(y) * a.rowBytes;
    const uint8_t* rb = b.pixels.data() + static_cast<size_t>(y) * b.rowBytes;
    if (std::memcmp(ra, rb, static_cast<size_t>(w) * 4u) != 0) {
      return false;
    }
  }
  return true;
}

// Count pixels with non-zero alpha - a cheap "did anything render" check.
size_t nonTransparentPixels(const RendererBitmap& bmp) {
  size_t count = 0;
  for (int y = 0; y < bmp.dimensions.y; ++y) {
    const uint8_t* row = bmp.pixels.data() + static_cast<size_t>(y) * bmp.rowBytes;
    for (int x = 0; x < bmp.dimensions.x; ++x) {
      if (row[x * 4 + 3] != 0) {
        ++count;
      }
    }
  }
  return count;
}

TEST_F(GeodePerfTest, GpuResidence_ReUploadsWhenDeviceChanges) {
  // Two INDEPENDENT headless devices (not the shared fixture device): this is
  // the "document crosses devices" scenario.
  auto deviceA = std::shared_ptr<geode::GeodeDevice>(geode::GeodeDevice::CreateHeadless());
  auto deviceB = std::shared_ptr<geode::GeodeDevice>(geode::GeodeDevice::CreateHeadless());
  ASSERT_TRUE(deviceA) << "GeodeDevice::CreateHeadless (A) failed";
  ASSERT_TRUE(deviceB) << "GeodeDevice::CreateHeadless (B) failed";
  ASSERT_NE(deviceA->deviceId(), deviceB->deviceId());

  ParseWarningSink sink = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(kSimpleShapesSvg, sink);
  ASSERT_FALSE(parsed.hasError()) << parsed.error().reason;
  // ONE document, rendered by both devices - its GeodeResidentPathComponents
  // are filled by device A and then re-encountered by device B.
  SVGDocument document = std::move(parsed.result());

  // Device A establishes residence and produces the reference image. Drive TWO
  // frames on the SAME renderer so its per-renderer frame index advances to 2.
  // This matters for the regression: `fillPathResident`'s same-frame gate
  // diverts a repeat draw whose frame index equals `slot.lastResidentFrame` to
  // the (correct) arena path. Device B is a fresh renderer whose first frame
  // index is 1; if device A had also stopped at frame index 1 the pre-fix gate
  // would coincidentally send device B down the arena path and mask the
  // cross-device bug. Advancing device A to frame index 2 guarantees device B
  // exercises the resident path on the pre-fix code.
  RendererGeode rendererA(deviceA);
  rendererA.draw(document);  // frame index 1: establishes residence on A.
  rendererA.draw(document);  // frame index 2: steady residence, lastResidentFrame=2.
  const RendererBitmap referenceA = rendererA.takeSnapshot();
  ASSERT_FALSE(referenceA.empty()) << "device A produced no snapshot";
  ASSERT_GT(nonTransparentPixels(referenceA), 0u)
      << "device A rendered nothing - fixture no longer exercises solid fills";

  // Device B (fresh renderer, frame index 1) renders the SAME document. Pre-fix,
  // the resident slots still hold device A's buffer + bind group; binding those
  // into device B's pass is a cross-device violation and the draws are dropped,
  // so the output diverges. With the fix the device-id mismatch forces a
  // re-upload onto device B and the output matches.
  RendererGeode rendererB(deviceB);
  rendererB.draw(document);  // frame index 1 != lastResidentFrame(2): resident path.
  const RendererBitmap resultB = rendererB.takeSnapshot();
  ASSERT_FALSE(resultB.empty()) << "device B produced no snapshot";

  EXPECT_TRUE(bitmapsEqual(referenceA, resultB))
      << "device B output diverged from device A: resident GPU resources from "
         "device A leaked into device B's render pass (cross-device residence). "
         "device A non-transparent px="
      << nonTransparentPixels(referenceA)
      << ", device B non-transparent px=" << nonTransparentPixels(resultB);

  // A render back on device A must still match (residence re-homes to A).
  RendererGeode rendererA2(deviceA);
  rendererA2.draw(document);
  const RendererBitmap reReferenceA = rendererA2.takeSnapshot();
  EXPECT_TRUE(bitmapsEqual(referenceA, reReferenceA))
      << "device A output changed after a device-B render round-trip";
}

}  // namespace
}  // namespace donner::svg
