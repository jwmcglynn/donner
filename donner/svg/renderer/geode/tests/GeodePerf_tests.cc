/// @file
/// Geode perf-counter assertions (design doc 0030 Milestone 0).
///
/// These tests are the durable regression signal for every optimization
/// milestone in `docs/design_docs/0030-geode_performance.md`. They assert
/// `GeodeCounters` ceilings — steady-state buffer / bindgroup / texture /
/// submit / path-encode counts — on representative SVG fixtures.
///
/// Counter ceilings are deterministic; wall-clock budgets are not. That's
/// why these tests (not the benchmark harness) are the CI gate.
///
/// Milestone 0 (this file, initial): ceilings match CURRENT observed
/// behaviour — the tests pass today. Each later milestone tightens the
/// ceiling(s) it targets; the `// M{N}:` comment beside each assertion
/// names the milestone that will change it.

#include <gtest/gtest.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
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
/// Exercises the pure `submitFillDraw` path — the Tier-1 hot path from
/// design 0030.
constexpr std::string_view kSimpleShapesSvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
  <rect x="10" y="10" width="80" height="80" fill="red"/>
  <circle cx="150" cy="50" r="40" fill="blue"/>
  <ellipse cx="100" cy="150" rx="60" ry="30" fill="green"/>
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

/// Dump counters to stderr so the observed values are visible in normal
/// test output (RecordProperty only surfaces in XML). Format keeps each
/// counter on its own column for easy diffing across milestones.
void printCounters(const char* label, const geode::GeodeCounters& c) {
  std::fprintf(
      stderr,
      "[GeodePerf] %-32s  pathEncodes=%4" PRIu64 "  bufferCreates=%5" PRIu64
      "  bindgroupCreates=%5" PRIu64 "  textureCreates=%3" PRIu64 "  submits=%3" PRIu64 "\n",
      label, c.pathEncodes, c.bufferCreates, c.bindgroupCreates, c.textureCreates, c.submits);
}

/// Read a file from disk. Returns the empty string on any I/O error —
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
  /// backed by this device — matches how production embedders wire things
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
  printCounters(::testing::UnitTest::GetInstance()->current_test_info()->name(), c);

  // Observed 2026-04-19 on macOS/Metal, M4 Pro:
  //   M0 baseline:        bufferCreates=13
  //   M1.d+e (ssbo+vb):   bufferCreates=7
  //   M1.f.1 (uniforms):  bufferCreates=5 (4 arenas + 1 readback,
  //                       arenas lazily grown — some frames only
  //                       touch 3 arenas)
  EXPECT_LE(c.pathEncodes, 5u);       // M2: target = 0 on unchanged-geometry frames.
  EXPECT_LE(c.bufferCreates, 8u);     // M1.f.2: target = 1 (readback only).
  EXPECT_LE(c.bindgroupCreates, 6u);  // M1.f.2: target <= #pipelines (3 today).
  EXPECT_LE(c.textureCreates, 6u);    // M4: target = 0 on unchanged-size repeat.
  EXPECT_LE(c.submits, 3u);           // M3: target = 1.
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
  printCounters(::testing::UnitTest::GetInstance()->current_test_info()->name(), c);

  // Observed 2026-04-19:
  //   M0 baseline:    bufferCreates=10 submits=4 textureCreates=10
  //   M1.d+e:         bufferCreates=10 submits=4 textureCreates=10
  //                   (three encoders from push/pop each allocate
  //                   their own arenas — 3×3 + 1 readback.)
  //   M3 (shared CE): bufferCreates=10 submits=2 textureCreates=10
  //                   (push/pop no longer forces a queue submit)
  EXPECT_LE(c.pathEncodes, 4u);       // M2: target = 0.
  EXPECT_LE(c.bufferCreates, 12u);    // M1.f.2 + future arena-share: target ~= 5.
  EXPECT_LE(c.bindgroupCreates, 6u);  // M1.f.2: target <= #pipelines.
  EXPECT_LE(c.textureCreates, 12u);   // M4: target = 0 on repeat same-size.
  EXPECT_LE(c.submits, 3u);           // M3: target = 2 steady-state (frame + readback).
}

// ---------------------------------------------------------------------------
// Fixture: lion.svg — the workhorse SVG used across Donner's test suites.
// Skipped gracefully if the file isn't bundled (e.g. unit test run without
// testdata deps).
// ---------------------------------------------------------------------------

TEST_F(GeodePerfTest, Lion_BaselineCeilings) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const std::string svg = readFile("donner/svg/renderer/testdata/lion.svg");
  if (svg.empty()) {
    GTEST_SKIP() << "testdata/lion.svg not readable — ensure the test target "
                 << "has testdata as a data dep.";
    return;
  }

  geode::GeodeCounters c = renderAndGetCounters(svg, device);

  RecordProperty("bufferCreates", std::to_string(c.bufferCreates));
  RecordProperty("bindgroupCreates", std::to_string(c.bindgroupCreates));
  RecordProperty("textureCreates", std::to_string(c.textureCreates));
  RecordProperty("submits", std::to_string(c.submits));
  RecordProperty("pathEncodes", std::to_string(c.pathEncodes));
  printCounters(::testing::UnitTest::GetInstance()->current_test_info()->name(), c);

  // Observed 2026-04-19:
  //   M0 baseline:        bufferCreates=529 (132 paths × 4 + 1 readback)
  //   M1.d+e:             bufferCreates=137 (3 arenas + 132 uniforms
  //                       + dummies + readback; 74% drop)
  //   M1.f.1 (uniforms):  bufferCreates=6 (4 arenas + 2 dummies +
  //                       readback; 98.9% total drop from M0)
  //   bindgroupCreates=132 (one per draw; M1.f.2 collapses to ~1).
  EXPECT_LE(c.pathEncodes, 200u);       // M2: target = 0.
  EXPECT_LE(c.bufferCreates, 10u);      // M1.f.2: target ~= 5 steady-state.
  EXPECT_LE(c.bindgroupCreates, 200u);  // M1.f.2: target <= #pipelines.
  EXPECT_LE(c.textureCreates, 6u);      // M4: target = 0.
  EXPECT_LE(c.submits, 3u);             // M3: target = 1.
}

// ---------------------------------------------------------------------------
// Double-render sanity: same renderer, two consecutive frames. Counters
// should reflect only the SECOND frame (beginFrame resets). This guards
// the reset path — if a future optimization accidentally persists state
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
  // reused across same-size frames (Milestone 4.1), so the second
  // frame's textureCreates should be strictly smaller.
  renderer.draw(document);
  (void)renderer.takeSnapshot();

  const auto secondCounters = renderer.lastFrameTimings().counters;

  // Second-frame counters are strictly this-frame only (beginFrame
  // resets). Since M2 (`GeodePathCacheComponent`) landed, an unchanged
  // second render does zero path encodes — the explicit assertion on
  // that invariant lives in `SimpleShapes_NoDirtyPath_ZeroEncodes`;
  // here we just confirm the reset path preserves the invariant across
  // counters and that render targets get reused (Milestone 4.1).
  EXPECT_EQ(secondCounters.pathEncodes, 0u);
  EXPECT_LE(secondCounters.pathEncodes, firstCounters.pathEncodes);
  EXPECT_LT(secondCounters.textureCreates, firstCounters.textureCreates)
      << "Second frame should create STRICTLY FEWER textures than the first "
      << "because render targets are reused at the same size.";
}

// ---------------------------------------------------------------------------
// Milestone 2: GeodePathCacheComponent — no-geometry-change ⇒ zero encodes.
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
/// the SECOND frame's counters. Used by the M2 zero-encode assertions.
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
  // First-frame counters intentionally discarded — we only care about the
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

  // M2 target: second frame does zero path encodes — three shapes hit the
  // cache. countPathEncode() is only called on cache miss.
  EXPECT_EQ(c.pathEncodes, 0u) << "Cache miss on an unchanged second render: one or more paths "
                                  "re-encoded despite zero geometry changes.";
}

TEST_F(GeodePerfTest, Moderate_NoDirtyPath_ZeroEncodes) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const geode::GeodeCounters c = countersForSecondRender(kModerateSvg, device);
  printCounters("Moderate_NoDirtyPath_ZeroEncodes (frame2)", c);

  // M2 target: zero encodes across both fill paths — confirms the cache
  // covers both `submitFillDraw` (opacity-layer path) and
  // `fillPathLinearGradient` (rounded-rect path).
  EXPECT_EQ(c.pathEncodes, 0u) << "Cache miss on unchanged second render: fill or gradient path "
                                  "re-encoded despite zero geometry changes.";
}

TEST_F(GeodePerfTest, Lion_NoDirtyPath_ZeroEncodes) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const std::string svg = readFile("donner/svg/renderer/testdata/lion.svg");
  if (svg.empty()) {
    GTEST_SKIP() << "testdata/lion.svg not readable — ensure the test target "
                 << "has testdata as a data dep.";
    return;
  }

  const geode::GeodeCounters c = countersForSecondRender(svg, device);
  printCounters("Lion_NoDirtyPath_ZeroEncodes (frame2)", c);

  // M2 target: 132 cached paths → zero re-encodes. This is the headline
  // assertion: the lion is our standard "many paths" stress fixture and
  // the M0 baseline showed 132 pathEncodes per frame. Driving that to 0
  // is the whole point of the cache.
  EXPECT_EQ(c.pathEncodes, 0u) << "Cache miss on unchanged second render of lion.svg: "
                                  "re-encoded paths despite zero geometry changes.";
}

TEST_F(GeodePerfTest, GhostscriptTiger_NoDirtyPath_ZeroEncodes) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const std::string svg = readFile("donner/svg/renderer/testdata/Ghostscript_Tiger.svg");
  if (svg.empty()) {
    GTEST_SKIP() << "testdata/Ghostscript_Tiger.svg not readable — ensure the "
                 << "test target has testdata as a data dep.";
    return;
  }

  const geode::GeodeCounters c = countersForSecondRender(svg, device);
  printCounters("GhostscriptTiger_NoDirtyPath_ZeroEncodes (frame2)", c);

  // M2 target: Tiger has strokes too, so this test also exercises the
  // stroke slot of `GeodePathCacheComponent` — a second render must not
  // re-run `Path::strokeToFill` nor re-encode the stroked outline.
  EXPECT_EQ(c.pathEncodes, 0u)
      << "Cache miss on unchanged second render of Ghostscript_Tiger.svg: "
         "fill- or stroke-slot cache missed despite zero geometry changes.";
}

// ---------------------------------------------------------------------------
// Milestone 4.2: Transient render-target pool — zero-alloc repeat render.
//
// Per design doc 0030 §M4.2: a size-keyed free list for layer / filter /
// mask / clip-mask scratch textures. Combined with M4.1 (target + MSAA
// reuse) and the per-encoder persistent dummy texture (M1), a repeat
// render of the same document at the same size should allocate zero
// textures on frame 2.
// ---------------------------------------------------------------------------

TEST_F(GeodePerfTest, SimpleShapes_NoDirtyPath_ZeroTextures) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const geode::GeodeCounters c = countersForSecondRender(kSimpleShapesSvg, device);
  printCounters("SimpleShapes_NoDirtyPath_ZeroTextures (frame2)", c);

  // Exercises only the main target / MSAA pair; no isolated layers.
  // Any texture allocation here means M4.1 regressed or a dummy is
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
  // layer allocates an RGBA8 resolve + 4× MSAA companion — M4.2 must
  // pool and reuse both.
  EXPECT_EQ(c.textureCreates, 0u)
      << "Isolated-layer texture leak on unchanged second render. "
         "Layer push/pop should draw from the M4.2 texture pool.";
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

}  // namespace
}  // namespace donner::svg
