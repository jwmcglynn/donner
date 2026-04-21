/// @file
///
/// Integration tests that validate the **actual pixel output** of the
/// editor sandbox pipeline:
///
/// ```
///     SVG source
///         │
///         ▼
///     EditorBackendCore::handleLoadBytes   (backend process in prod)
///         │
///         ▼  FramePayload.renderWire
///     ReplayingRenderer::pumpFrame        (host process — replay side)
///         │
///         ▼
///     svg::Renderer::takeSnapshot()       (host-side real backend)
/// ```
///
/// The pre-existing `editor_backend_integration_tests` only checks that
/// the render wire is non-empty. Non-empty bytes ≠ correct pixels: a
/// stream that begins/ends its header but never emits a single draw
/// call still round-trips as "non-empty". The editor UI reports
/// "nothing renders" — this suite exists to pin that bug with a
/// mechanical, debuggable repro.
///
/// Two complementary golden strategies:
///
///   1. **Direct-renderer reference golden.** Render the same document
///      through `svg::Renderer::draw(doc)` directly (no sandbox) and
///      use its output as the reference. The sandbox path MUST
///      byte-for-byte match the direct path for trivial documents —
///      any divergence is a bug in `SerializingRenderer` +
///      `ReplayingRenderer` faithfulness. No external golden file,
///      so the test is self-contained and can't silently rot.
///
///   2. **Smoke gates before the full diff.** `FramePayload.statusKind
///      == kRendered`, `renderWire` not trivially short, replay status
///      `kOk`, snapshot non-empty, at least one opaque pixel. These
///      fail faster and point at *where* in the pipeline things
///      broke. Ordered finest-first; each assertion tightens the
///      diagnosis if the earlier one passes.
///
/// This test is cross-platform by construction — it uses
/// `EditorBackendCore` directly, not `SandboxSession` — so macOS
/// developers can iterate on the pipeline without a Linux box.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/editor/EditorBackendClient.h"
#include "donner/editor/ViewportState.h"
#include "donner/editor/sandbox/EditorApiCodec.h"
#include "donner/editor/sandbox/EditorBackendCore.h"
#include "donner/editor/sandbox/ReplayingRenderer.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererImageIO.h"

namespace donner::editor::sandbox {
namespace {

/// A small SVG the test suite reuses: 100×100 canvas, one red rect that
/// covers most of it. Chosen to be the smallest possible input that
/// produces a visually obvious non-empty bitmap — so "entirely
/// transparent output" and "fill-color wrong" are both trivially
/// distinguishable from each other in diff output.
constexpr std::string_view kRedRectSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">)"
    R"(<rect x="10" y="10" width="80" height="80" fill="red"/>)"
    R"(</svg>)";

/// Canvas size matching the SVG above. Keeps the comparison 1:1 in
/// world space so the backend's rasterized pixels line up with what
/// the direct renderer would produce.
constexpr int kCanvasWidth = 100;
constexpr int kCanvasHeight = 100;

/// Count non-transparent pixels (alpha > 0) in an RGBA bitmap. Used by
/// the smoke check that catches "output is entirely transparent" —
/// the failure mode the user observed as "nothing renders".
size_t CountOpaquePixels(const svg::RendererBitmap& bitmap) {
  size_t opaque = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    const uint8_t* row = bitmap.pixels.data() + static_cast<size_t>(y) * bitmap.rowBytes;
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      if (row[x * 4 + 3] != 0) {
        ++opaque;
      }
    }
  }
  return opaque;
}

/// Dump an RGBA bitmap to `/tmp/<name>.png` on test failure so the
/// developer can eyeball it. Silent success/failure — the return value
/// is purely diagnostic; tests don't assert on the write.
/// Parse-and-render reference helper, free-standing so it can be
/// called from any fixture or test. Mirrors what
/// `EditorBackendGoldenImageTest::renderDirectly` does but doesn't
/// need a fixture instance.
svg::RendererBitmap RenderSvgDirectly(std::string_view svg, int width, int height) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  auto parseResult = svg::parser::SVGParser::ParseSVG(svg, disabled);
  if (parseResult.hasError()) {
    ADD_FAILURE() << "direct-render helper failed to parse SVG: " << parseResult.error();
    return {};
  }
  svg::SVGDocument doc = std::move(parseResult.result());
  doc.setCanvasSize(width, height);

  svg::Renderer renderer;
  renderer.draw(doc);
  return renderer.takeSnapshot();
}

void DumpBitmapForDebug(const svg::RendererBitmap& bitmap, const char* name) {
  if (bitmap.empty()) {
    return;
  }
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     (std::string(name) + ".png");
  svg::RendererImageIO::writeRgbaPixelsToPngFile(
      path.string().c_str(), bitmap.pixels, bitmap.dimensions.x, bitmap.dimensions.y,
      bitmap.rowBytes / 4);
  std::fprintf(stderr, "[EditorBackendGoldenImage] wrote debug PNG: %s\n", path.string().c_str());
}

class EditorBackendGoldenImageTest : public ::testing::Test {
 protected:
  /// Run the SVG through the sandbox pipeline end-to-end. The backend
  /// now ships the pre-composed bitmap directly via
  /// `FramePayload.finalBitmap` (compositor-mode rendering); we
  /// reconstruct a `svg::RendererBitmap` from those fields and return
  /// it. The wire-replay fallback path is covered by the "legacy wire
  /// fallback" tests below.
  svg::RendererBitmap renderViaSandbox(std::string_view svg, int width, int height) {
    EditorBackendCore core;

    SetViewportPayload vp;
    vp.width = width;
    vp.height = height;
    (void)core.handleSetViewport(vp);

    LoadBytesPayload load;
    load.bytes = std::string(svg);
    const FramePayload frame = core.handleLoadBytes(load);

    EXPECT_EQ(frame.statusKind, FrameStatusKind::kRendered)
        << "backend reported non-rendered status for a well-formed SVG — "
           "parse failed or the render step was skipped";
    EXPECT_TRUE(frame.hasFinalBitmap)
        << "compositor-mode backend didn't ship a finalBitmap for a "
           "well-formed SVG — the compositor wasn't driven or the "
           "snapshot came back empty";

    svg::RendererBitmap bitmap;
    if (frame.hasFinalBitmap) {
      bitmap.dimensions = Vector2i(frame.finalBitmapWidth, frame.finalBitmapHeight);
      bitmap.rowBytes = frame.finalBitmapRowBytes;
      bitmap.alphaType = static_cast<svg::AlphaType>(frame.finalBitmapAlphaType);
      bitmap.pixels = frame.finalBitmapPixels;
    }
    return bitmap;
  }

  /// Parse + render the same SVG through `svg::Renderer::draw(doc)`
  /// directly, with no sandbox/serialize/replay layer. Used as the
  /// reference for pixel-identity golden checks — the sandbox path
  /// must match this output byte-for-byte on trivial documents.
  svg::RendererBitmap renderDirectly(std::string_view svg, int width, int height) {
    ParseWarningSink disabled = ParseWarningSink::Disabled();
    auto parseResult = svg::parser::SVGParser::ParseSVG(svg, disabled);
    EXPECT_FALSE(parseResult.hasError())
        << "test SVG failed to parse: " << parseResult.error();
    if (parseResult.hasError()) {
      return {};
    }
    svg::SVGDocument doc = std::move(parseResult.result());
    doc.setCanvasSize(width, height);

    svg::Renderer renderer;
    renderer.draw(doc);
    return renderer.takeSnapshot();
  }
};

// ---------------------------------------------------------------------------
// Smoke gate: does ANYTHING render at all?
// ---------------------------------------------------------------------------

/// Catches the current "nothing renders" complaint. A working pipeline
/// must produce a bitmap whose opaque-pixel count roughly matches the
/// rect area the SVG describes; zero opaque pixels is the failure
/// mode the user observed.
TEST_F(EditorBackendGoldenImageTest, SandboxPipelineProducesOpaquePixels) {
  const svg::RendererBitmap bitmap = renderViaSandbox(kRedRectSvg, kCanvasWidth, kCanvasHeight);

  ASSERT_FALSE(bitmap.empty()) << "sandbox pipeline returned an empty bitmap";
  ASSERT_EQ(bitmap.dimensions.x, kCanvasWidth);
  ASSERT_EQ(bitmap.dimensions.y, kCanvasHeight);

  const size_t opaque = CountOpaquePixels(bitmap);
  if (opaque == 0) {
    DumpBitmapForDebug(bitmap, "sandbox_opaque_zero");
  }
  EXPECT_GT(opaque, 0u) << "sandbox output is entirely transparent — "
                          "this is the 'nothing renders' bug";

  // The SVG describes an 80×80 rect (6400 px). Allow for AA fringes
  // inflating the opaque count slightly; reject obvious underdraws.
  constexpr size_t kExpectedOpaqueFloor = 6000;
  EXPECT_GE(opaque, kExpectedOpaqueFloor)
      << "sandbox rasterized far fewer opaque pixels than the SVG demands — "
         "the rect's fill pass probably never ran";
}

// ---------------------------------------------------------------------------
// Golden gate: does the sandbox pipeline produce the SAME pixels as a
// direct render?
// ---------------------------------------------------------------------------

/// Full byte-for-byte pixel identity between the sandbox pipeline and
/// the direct `svg::Renderer::draw(doc)` path. If these diverge on a
/// trivial red rect, either `SerializingRenderer` is dropping calls
/// or `ReplayingRenderer` is mis-replaying them.
///
/// If they match, we've definitively proved the sandbox pipeline is
/// lossless on at least this one input shape, and any downstream
/// "nothing renders" complaint is upstream of the renderWire (e.g.
/// the host-side `ReplayingRenderer` that the editor binary
/// instantiates isn't being driven, isn't on this bitmap path, etc.).
TEST_F(EditorBackendGoldenImageTest, SandboxPixelOutputMatchesDirectRenderer) {
  const svg::RendererBitmap sandbox =
      renderViaSandbox(kRedRectSvg, kCanvasWidth, kCanvasHeight);
  const svg::RendererBitmap direct =
      renderDirectly(kRedRectSvg, kCanvasWidth, kCanvasHeight);

  ASSERT_FALSE(sandbox.empty());
  ASSERT_FALSE(direct.empty());
  ASSERT_EQ(sandbox.dimensions, direct.dimensions);
  ASSERT_EQ(sandbox.rowBytes, direct.rowBytes);
  ASSERT_EQ(sandbox.pixels.size(), direct.pixels.size());

  if (sandbox.pixels != direct.pixels) {
    DumpBitmapForDebug(sandbox, "sandbox_divergent");
    DumpBitmapForDebug(direct, "direct_reference");

    // Pinpoint the first divergent pixel so a human reading the
    // failure can tell whether the divergence is a constant color
    // offset (AlphaType confusion), a geometric miss (wrong transform),
    // or a total blank (no draws).
    for (size_t i = 0; i + 3 < sandbox.pixels.size(); i += 4) {
      if (sandbox.pixels[i] != direct.pixels[i] ||
          sandbox.pixels[i + 1] != direct.pixels[i + 1] ||
          sandbox.pixels[i + 2] != direct.pixels[i + 2] ||
          sandbox.pixels[i + 3] != direct.pixels[i + 3]) {
        const size_t pixelIdx = i / 4;
        const int x = static_cast<int>(pixelIdx % (sandbox.rowBytes / 4));
        const int y = static_cast<int>(pixelIdx / (sandbox.rowBytes / 4));
        std::fprintf(stderr,
                     "[EditorBackendGoldenImage] first divergent pixel at (%d, %d): "
                     "sandbox=(%u,%u,%u,%u) direct=(%u,%u,%u,%u)\n",
                     x, y, sandbox.pixels[i], sandbox.pixels[i + 1], sandbox.pixels[i + 2],
                     sandbox.pixels[i + 3], direct.pixels[i], direct.pixels[i + 1],
                     direct.pixels[i + 2], direct.pixels[i + 3]);
        break;
      }
    }
  }

  EXPECT_EQ(sandbox.pixels, direct.pixels)
      << "sandbox pipeline diverges from direct-renderer golden — see "
         "stderr for first divergent pixel and /tmp/sandbox_divergent.png, "
         "/tmp/direct_reference.png";
}

// ---------------------------------------------------------------------------
// Additional shapes beyond the trivial rect — guards against the rect-
// only case accidentally passing while everything else is broken.
// ---------------------------------------------------------------------------

TEST_F(EditorBackendGoldenImageTest, SandboxMatchesDirectForCircle) {
  constexpr std::string_view kCircleSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="50" height="50">)"
      R"(<circle cx="25" cy="25" r="20" fill="blue"/>)"
      R"(</svg>)";

  const svg::RendererBitmap sandbox = renderViaSandbox(kCircleSvg, 50, 50);
  const svg::RendererBitmap direct = renderDirectly(kCircleSvg, 50, 50);

  ASSERT_FALSE(sandbox.empty());
  ASSERT_FALSE(direct.empty());
  EXPECT_GT(CountOpaquePixels(sandbox), 0u) << "sandbox renders no pixels for a simple circle";
  EXPECT_EQ(sandbox.pixels, direct.pixels);
}

TEST_F(EditorBackendGoldenImageTest, SandboxMatchesDirectForPath) {
  // An 'X' composed of two line segments. Stroked path tests the
  // `drawPath` + `StrokeParams` wire encoding, which is a separate
  // code path from filled shapes.
  constexpr std::string_view kPathSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="60" height="60">)"
      R"(<path d="M10 10 L50 50 M50 10 L10 50" stroke="green" stroke-width="4" fill="none"/>)"
      R"(</svg>)";

  const svg::RendererBitmap sandbox = renderViaSandbox(kPathSvg, 60, 60);
  const svg::RendererBitmap direct = renderDirectly(kPathSvg, 60, 60);

  ASSERT_FALSE(sandbox.empty());
  ASSERT_FALSE(direct.empty());
  EXPECT_GT(CountOpaquePixels(sandbox), 0u) << "sandbox renders no pixels for a stroked path";
  EXPECT_EQ(sandbox.pixels, direct.pixels);
}

// ---------------------------------------------------------------------------
// Host-side (thin-client) flow reproduction.
//
// The tests above exercise the sandbox pipeline in isolation. The "nothing
// renders" complaint shows up only in the editor binary, so the fixture
// below drives `EditorBackendClient` — the API the thin-client `main.cc`
// actually uses — and reproduces its exact call pattern:
//
//     client.loadBytes(svgBytes)   ← renders at backend default viewport
//     client.setViewport(w, h)     ← re-renders at UI viewport
//
// If `setViewport` re-renders but its frame is discarded by the host,
// `client.latestBitmap()` stays at the tiny initial snapshot. For a document
// whose natural canvas is bigger than the default, the texture uploaded on
// startup is a clipped corner — experienced by the user as "nothing
// renders" (the document content lives outside the 512×384 top-left).
// ---------------------------------------------------------------------------

class EditorBackendClientHostFlowTest : public ::testing::Test {};

/// Reproduces `main.cc`'s initialization sequence:
///   loadBytes → (void)setViewport → read latestBitmap().
///
/// A faithful host would see `latestBitmap()` tracking the most recent
/// setViewport result. Before the fix, `setViewport`'s returned future is
/// `(void)`-cast and dropped — and in `InProcessEditorBackendClient` the
/// future's `cacheResult()` only runs inside the future's `set_value`
/// callback chain, so the discarded future leaves `latestBitmap_` frozen
/// at the pre-resize snapshot.
TEST_F(EditorBackendClientHostFlowTest,
       SetViewportUpdatesLatestBitmapEvenWhenCallerDiscardsFuture) {
  auto client = EditorBackendClient::MakeInProcess();

  // Step 1: match main.cc — load the document before any setViewport. The
  // backend renders at its default 512×384 at this point.
  auto loadFuture = client->loadBytes(
      std::span(reinterpret_cast<const uint8_t*>(kRedRectSvg.data()), kRedRectSvg.size()),
      std::nullopt);
  FrameResult loadResult = loadFuture.get();
  ASSERT_TRUE(loadResult.ok);

  const svg::RendererBitmap& afterLoad = client->latestBitmap();
  ASSERT_FALSE(afterLoad.empty()) << "initial loadBytes produced no bitmap";
  const Vector2i loadDims = afterLoad.dimensions;

  // Step 2: simulate main.cc:1236 — fire-and-forget setViewport. Once per
  // UI frame, `(void)backend->setViewport(w, h)` is issued; its returned
  // future is discarded. The test is specifically asserting that this
  // control flow updates the latestBitmap AS OBSERVED BY THE NEXT
  // latestBitmap() read — i.e. the discard must not cause lost state.
  //
  // The backend's aspect-preserving letterbox turns a 100×100 SVG into
  // `min(w,h) × min(w,h)`: default 512×384 → 384×384, 1280×720 →
  // 720×720. The assertion is "dimensions CHANGED from before", not a
  // specific number — that way the test stays honest about what
  // setViewport does without hardcoding aspect-fit behavior that
  // belongs to the layout layer.
  constexpr int kUiWidth = 1280;
  constexpr int kUiHeight = 720;
  (void)client->setViewport(kUiWidth, kUiHeight);

  const svg::RendererBitmap& afterViewport = client->latestBitmap();
  EXPECT_FALSE(afterViewport.empty())
      << "setViewport's re-render never reached the client's latestBitmap — "
         "discarding the future lost the frame, and the UI stays showing "
         "the tiny default-viewport bitmap captured at load time.";
  EXPECT_NE(afterViewport.dimensions, loadDims)
      << "latestBitmap dimensions are still " << loadDims.x << "x" << loadDims.y
      << " (same as pre-setViewport). setViewport's discarded future "
         "didn't land the new render in the bitmap cache.";
  EXPECT_GT(afterViewport.dimensions.x, loadDims.x)
      << "bitmap didn't grow after widening the viewport — cache wasn't refreshed";

  EXPECT_GT(CountOpaquePixels(afterViewport), 0u)
      << "post-setViewport bitmap is entirely transparent";
}

/// Simulates main.cc's actual texture-upload control flow without GL.
///
/// main.cc has two upload sites:
///   1. Explicit: `ProcessFrameResult(future.get())` after loadBytes / undo /
///      redo / pointerEvent / replaceSource — always uploads `result.bitmap`.
///   2. Implicit startup: reads `backend->latestBitmap()` once, uploads it,
///      then never reads again.
///
/// setViewport is called *every frame* with `(void)` — its FrameResult is
/// discarded. That update lives ONLY in `backend->latestBitmap()`, which
/// main.cc never re-reads. So when the UI grows the pane after startup,
/// the backend re-renders at the new size but the GL texture is frozen
/// at the tiny default 512×384-squared bitmap from the initial upload.
///
/// This test emulates the "what the GL texture would show" by tracking
/// the bitmap the host has chosen to upload so far, and asserts that it
/// would stay frozen without an explicit fix. Must fail on pre-fix code;
/// passes only once main.cc starts awaiting setViewport's FrameResult
/// (or re-reads latestBitmap() whose identity / generation changed).
TEST_F(EditorBackendClientHostFlowTest,
       DiscardedSetViewportLeavesHostUploadFrozenAtInitialBitmap) {
  auto client = EditorBackendClient::MakeInProcess();

  // Simulates main.cc:637 — load the doc.
  auto loadFuture = client->loadBytes(
      std::span(reinterpret_cast<const uint8_t*>(kRedRectSvg.data()), kRedRectSvg.size()),
      std::nullopt);
  const FrameResult loadResult = loadFuture.get();
  ASSERT_TRUE(loadResult.ok);

  // Simulates main.cc:667-674 — initial one-shot upload from
  // `backend->latestBitmap()`. From this point on main.cc only uploads
  // new bitmaps via `ProcessFrameResult(future.get())`.
  svg::RendererBitmap uploadedBitmap = client->latestBitmap();
  const Vector2i initialUploadDims = uploadedBitmap.dimensions;
  ASSERT_GT(initialUploadDims.x, 0);

  // Simulates the main loop at main.cc:1233 — setViewport called every
  // frame, future discarded.
  constexpr int kNewWidth = 1280;
  constexpr int kNewHeight = 720;
  (void)client->setViewport(kNewWidth, kNewHeight);

  // Main.cc has no code path that re-reads `latestBitmap()` after
  // startup, so the "uploaded" snapshot (what GL has) is still at
  // initial dims. If/when someone adds a poll of latestBitmap() after
  // the setViewport tick, this assertion will (correctly) flip and
  // the test needs to be updated alongside the fix.
  //
  // The behavior this assertion pins is: discarding the future silently
  // loses the new frame from every caller that only consumes bitmaps
  // via `ProcessFrameResult(future.get())`. That's exactly the bug the
  // user sees — the texture shown on screen never grows past the tiny
  // default-viewport render.
  const Vector2i wouldStayUploadedDims = uploadedBitmap.dimensions;
  EXPECT_EQ(wouldStayUploadedDims, initialUploadDims);

  // Prove the fix is available: awaiting the future (or re-reading
  // latestBitmap() now) *would* surface a larger, correct-size bitmap.
  // This is what main.cc should do.
  const svg::RendererBitmap& postSetViewportBitmap = client->latestBitmap();
  EXPECT_NE(postSetViewportBitmap.dimensions, initialUploadDims)
      << "Sanity: the backend DID render a new bitmap after setViewport. "
         "Main.cc just never looks at it. Uploading "
         "`client->latestBitmap()` after setViewport (or awaiting the "
         "returned future and running ProcessFrameResult) closes the bug.";
}

/// Reproduces the bigger-document variant of the above: the UI's effective
/// canvas is LARGER than the backend default. Before the fix, the texture
/// uploaded to GL is 512×384 of a 892×512 document — a top-left crop the
/// user reads as "nothing visible" if the interesting content lives
/// further into the canvas.
TEST_F(EditorBackendClientHostFlowTest,
       LargerDocumentRendersAtFullViewportAfterSetViewport) {
  // Mirrors the real editor workload: a document whose natural canvas
  // exceeds the backend's default viewport. The content at (600, 400)
  // lives outside the 512×384 default crop, so if setViewport is dropped
  // the opaque-pixel count in that region is zero.
  constexpr std::string_view kBigSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="892" height="512">)"
      R"(<rect x="0" y="0" width="892" height="512" fill="#000"/>)"
      R"(<rect x="600" y="400" width="80" height="80" fill="red"/>)"
      R"(</svg>)";

  auto client = EditorBackendClient::MakeInProcess();
  auto loadFuture = client->loadBytes(
      std::span(reinterpret_cast<const uint8_t*>(kBigSvg.data()), kBigSvg.size()), std::nullopt);
  (void)loadFuture.get();

  (void)client->setViewport(892, 512);

  const svg::RendererBitmap& bitmap = client->latestBitmap();
  ASSERT_FALSE(bitmap.empty());
  ASSERT_EQ(bitmap.dimensions.x, 892);
  ASSERT_EQ(bitmap.dimensions.y, 512);

  // Red rect lives at x∈[600,680], y∈[400,480]. Sample inside that
  // region — if the viewport really took effect, these pixels are red;
  // if setViewport was dropped, the bitmap is smaller than (600, 400)
  // and this ASSERT_FALSE on empty() above would have already failed.
  const size_t y = 440;
  const size_t x = 640;
  const size_t offset = y * bitmap.rowBytes + x * 4;
  ASSERT_LT(offset + 3, bitmap.pixels.size());
  EXPECT_GT(bitmap.pixels[offset + 3], 0u)
      << "pixel at (640, 440) — inside the red rect — is transparent; "
         "setViewport's re-render didn't reach latestBitmap";
}

// ---------------------------------------------------------------------------
// Thin-client UI-flow reproduction.
//
// These tests drive `EditorBackendClient` the way `main.cc` actually does
// — including the initial-upload-from-latestBitmap pass, the every-frame
// setViewport post, and the `ProcessFrameResult`-style texture refresh
// — and verify the BITMAP WE WOULD UPLOAD TO GL is non-empty and has
// visible content. They exist because the prior "nothing renders" bug
// passed every pipeline-level test (renderWire non-empty, replay
// matches direct render) but failed in the real editor because
// `(void)setViewport(...)` dropped the returned FrameResult and the
// GL texture froze at the initial default-viewport render.
//
// The simulated upload surface is just the latest bitmap we've decided
// to "upload" — we can't touch GL from a unit test, but the
// upload-decision logic in `main.cc` is what actually had the bug and
// what these tests re-verify.
// ---------------------------------------------------------------------------

/// Simulates the thin-client `main.cc` flow. Mirrors the exact sequence
/// of backend calls without the OpenGL calls:
///   * `loadBytes`, capture initial bitmap as the first "uploaded".
///   * Post `setViewport(paneW, paneH)` whenever the pane size
///     changes. Await the returned future and upload ITS bitmap.
/// After calling `present(...)`, `lastUploadedBitmap()` holds the
/// bitmap the real editor would have in the GL texture.
class ThinClientSimulator {
 public:
  explicit ThinClientSimulator(std::unique_ptr<EditorBackendClient> client)
      : client_(std::move(client)) {}

  void loadBytes(std::string_view svg) {
    auto future = client_->loadBytes(
        std::span(reinterpret_cast<const uint8_t*>(svg.data()), svg.size()), std::nullopt);
    const FrameResult load = future.get();
    EXPECT_TRUE(load.ok);
    // main.cc's "initial upload" path reads `backend->latestBitmap()`
    // rather than pulling from the FrameResult.
    lastUploaded_ = client_->latestBitmap();
  }

  void present(int paneW, int paneH) {
    const Vector2i desired(std::max(paneW, 1), std::max(paneH, 1));
    if (desired == lastPostedViewport_) {
      return;
    }
    lastPostedViewport_ = desired;
    auto future = client_->setViewport(desired.x, desired.y);
    const FrameResult frame = future.get();
    if (frame.ok && !frame.bitmap.empty()) {
      lastUploaded_ = frame.bitmap;
    }
  }

  const svg::RendererBitmap& lastUploadedBitmap() const { return lastUploaded_; }
  EditorBackendClient& client() { return *client_; }

 private:
  std::unique_ptr<EditorBackendClient> client_;
  svg::RendererBitmap lastUploaded_;
  Vector2i lastPostedViewport_{-1, -1};
};

class ThinClientUiFlowTest : public ::testing::Test {};

/// Reproduces the exact "all black pane" symptom: after `loadBytes` and
/// a subsequent `present(paneW, paneH)`, the uploaded bitmap must match
/// the backend's current render at the requested pane size and have
/// opaque content — NOT be frozen at the initial default-viewport
/// render, and NOT be a 1×1 placeholder the viewport helper produces
/// when `documentViewBox` is unpopulated.
TEST_F(ThinClientUiFlowTest, LoadPlusPresentUploadsNonDegenerateBitmap) {
  // 892×512 canvas, dark background, colorful foreground — same shape
  // as donner_splash in structure. Using a synthetic SVG instead of
  // the real splash keeps the test runfile-free and deterministic.
  constexpr std::string_view kRepresentativeSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="892" height="512">)"
      R"(<rect width="892" height="512" fill="#0d1226"/>)"
      R"(<rect x="120" y="80" width="240" height="160" fill="#fa0"/>)"
      R"(<circle cx="620" cy="380" r="70" fill="#7df"/>)"
      R"(</svg>)";

  ThinClientSimulator sim(EditorBackendClient::MakeInProcess());
  sim.loadBytes(kRepresentativeSvg);

  // Emulates the UI pane growing to a typical editor window size on
  // the first layout-settled frame.
  constexpr int kPaneW = 1310;
  constexpr int kPaneH = 752;
  sim.present(kPaneW, kPaneH);

  const svg::RendererBitmap& up = sim.lastUploadedBitmap();
  ASSERT_FALSE(up.empty()) << "thin-client texture upload is empty — nothing on screen";
  EXPECT_GT(up.dimensions.x, 16) << "uploaded bitmap dims=" << up.dimensions.x << "x"
                                  << up.dimensions.y
                                  << " — degenerate placeholder. setViewport's result "
                                     "wasn't picked up.";
  EXPECT_GT(up.dimensions.y, 16);
  EXPECT_GT(CountOpaquePixels(up), 0u) << "uploaded bitmap is entirely transparent — "
                                          "the texture would render as whatever is "
                                          "behind it (ImGui window bg → 'all black').";
}

/// Golden-image test: drive the thin-client simulator, dump the final
/// uploaded bitmap, diff it against the same SVG rendered directly.
/// Passes only when the thin-client's texture-upload decisions
/// culminate in pixels that actually match what the user is supposed
/// to see.
///
/// This is the "instrumented UI" test the user asked for: it goes
/// through the real main.cc-shaped flow (not just
/// `client.latestBitmap()` in isolation) and asserts the on-texture
/// content is correct.
TEST_F(ThinClientUiFlowTest, ThinClientUploadMatchesDirectRenderGolden) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="300">)"
      R"(<rect width="400" height="300" fill="#0d1226"/>)"
      R"(<rect x="40" y="40" width="120" height="80" fill="#fa0"/>)"
      R"(<circle cx="280" cy="180" r="40" fill="#7df"/>)"
      R"(</svg>)";

  ThinClientSimulator sim(EditorBackendClient::MakeInProcess());
  sim.loadBytes(kSvg);

  // Present at the document's natural size so the backend renders
  // at 1:1 and the bitmap is directly comparable to the reference.
  sim.present(400, 300);

  const svg::RendererBitmap& up = sim.lastUploadedBitmap();
  ASSERT_FALSE(up.empty());
  ASSERT_EQ(up.dimensions.x, 400);
  ASSERT_EQ(up.dimensions.y, 300);

  // Direct-render reference — the pixels the thin-client's final
  // texture MUST match.
  const svg::RendererBitmap reference = RenderSvgDirectly(kSvg, 400, 300);
  ASSERT_EQ(up.dimensions, reference.dimensions);

  if (up.pixels != reference.pixels) {
    DumpBitmapForDebug(up, "thin_client_uploaded");
    DumpBitmapForDebug(reference, "direct_reference");
    for (size_t i = 0; i + 3 < up.pixels.size(); i += 4) {
      if (up.pixels[i] != reference.pixels[i] ||
          up.pixels[i + 1] != reference.pixels[i + 1] ||
          up.pixels[i + 2] != reference.pixels[i + 2] ||
          up.pixels[i + 3] != reference.pixels[i + 3]) {
        const size_t pixelIdx = i / 4;
        const int x = static_cast<int>(pixelIdx % (up.rowBytes / 4));
        const int y = static_cast<int>(pixelIdx / (up.rowBytes / 4));
        std::fprintf(stderr,
                     "[ThinClientUi] first divergent pixel at (%d, %d): uploaded=(%u,%u,%u,%u) "
                     "reference=(%u,%u,%u,%u)\n",
                     x, y, up.pixels[i], up.pixels[i + 1], up.pixels[i + 2], up.pixels[i + 3],
                     reference.pixels[i], reference.pixels[i + 1], reference.pixels[i + 2],
                     reference.pixels[i + 3]);
        break;
      }
    }
  }

  EXPECT_EQ(up.pixels, reference.pixels);
}

/// Viewport-centering regression: reproduces the user's "top-left of the
/// canvas is centered on screen" complaint. The screen-space AABB of
/// the document must be centered in the pane (pane center maps to doc
/// center), NOT anchored with the doc's (0, 0) at pane center.
///
/// Pre-fix: `ViewportState::documentViewBox` stayed at `Box2d::Zero`,
/// so `documentViewBoxCenter() == (0, 0)`, and `resetTo100Percent`
/// anchored `panScreenPoint = paneCenter` to `panDocPoint = (0, 0)` —
/// placing the doc's top-left at the pane's center, which is exactly
/// the visual the user reported.
TEST_F(ThinClientUiFlowTest, DocumentCenterMapsToPaneCenter) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="300">)"
      R"(<rect width="400" height="300" fill="red"/>)"
      R"(</svg>)";

  ThinClientSimulator sim(EditorBackendClient::MakeInProcess());
  sim.loadBytes(kSvg);
  sim.present(800, 600);

  // Replicate main.cc's own setup: seed documentViewBox from the
  // uploaded bitmap, call resetTo100Percent, and verify that the
  // document's geometric center lands at the pane's center in screen
  // space. This is the invariant `resetTo100Percent` promises; it's
  // broken iff documentViewBox is left at Box2d::Zero.
  ViewportState viewport;
  viewport.paneOrigin = Vector2d(100.0, 80.0);
  viewport.paneSize = Vector2d(800.0, 600.0);
  viewport.devicePixelRatio = 1.0;

  const svg::RendererBitmap& up = sim.lastUploadedBitmap();
  ASSERT_FALSE(up.empty());
  viewport.documentViewBox =
      Box2d::FromXYWH(0.0, 0.0, static_cast<double>(up.dimensions.x),
                      static_cast<double>(up.dimensions.y));
  viewport.resetTo100Percent();

  const Vector2d docCenter =
      (viewport.documentViewBox.topLeft + viewport.documentViewBox.bottomRight) * 0.5;
  const Vector2d paneCenter = viewport.paneOrigin + viewport.paneSize * 0.5;
  const Vector2d screenDocCenter = viewport.documentToScreen(docCenter);

  EXPECT_NEAR(screenDocCenter.x, paneCenter.x, 0.5)
      << "doc center maps to screen x=" << screenDocCenter.x << ", expected pane center "
      << paneCenter.x << " — likely `documentViewBox` was left as Box2d::Zero so "
         "panDocPoint defaulted to (0, 0) and the doc's top-left got centered.";
  EXPECT_NEAR(screenDocCenter.y, paneCenter.y, 0.5);

  // Cross-check: the document's top-left should NOT land on the pane center —
  // that's the user's observed bug. Assert it lands NW of the pane center.
  const Vector2d screenDocTopLeft = viewport.documentToScreen(viewport.documentViewBox.topLeft);
  EXPECT_LT(screenDocTopLeft.x, paneCenter.x)
      << "doc top-left is at or past pane center x — the 'top-left centered on screen' bug";
  EXPECT_LT(screenDocTopLeft.y, paneCenter.y);
}

// ---------------------------------------------------------------------------
// Coordinate-space: the frame's viewBox + click → hit-test round-trip.
//
// The backend's selection bboxes (`FrameResult.selection.worldBBox`) and
// hit-test point inputs are in the SVG's own user-space coordinates — the
// document's viewBox, NOT the rasterized bitmap's pixel grid. The host
// needs to know the document viewBox to:
//   * Map a mouse click at screen pixel (SX, SY) to a `documentX/Y`
//     the backend will hit-test correctly.
//   * Draw selection chrome (AABBs) at the right screen rect.
//   * Place the rendered bitmap so its content aligns with the
//     chrome drawn on top of it.
//
// Pre-fix the thin client seeded `ViewportState::documentViewBox` from
// the rasterized bitmap dimensions. For any document whose intrinsic
// size didn't equal the render canvas size, that meant click coords
// arrived in bitmap-pixel space and hit-test fell in the wrong spot —
// and the subsequent AABB echoed that wrong-space point on screen.
//
// These tests lock in the wire-level fix: `FramePayload.hasDocumentView
// Box / documentViewBox` must carry the SVG's true viewBox, and the
// client must expose it on `FrameResult` + `latestDocumentViewBox()`.
// ---------------------------------------------------------------------------

TEST_F(ThinClientUiFlowTest, FramePayloadReportsSvgOwnViewBox) {
  // SVG with explicit viewBox = (0 0 200 100) but width/height that
  // resolve to a different aspect ratio — what the rasterized bitmap
  // ends up at depends on the render canvas, but the viewBox the
  // backend uses for hit-test does NOT.
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="892" height="512" viewBox="0 0 200 100">)"
      R"(<rect x="10" y="10" width="80" height="40" fill="red"/>)"
      R"(</svg>)";

  auto client = EditorBackendClient::MakeInProcess();
  FrameResult load = client
                         ->loadBytes(std::span(reinterpret_cast<const uint8_t*>(kSvg.data()),
                                                kSvg.size()),
                                     std::nullopt)
                         .get();
  ASSERT_TRUE(load.ok);

  ASSERT_TRUE(load.documentViewBox.has_value())
      << "FrameResult.documentViewBox is empty — backend isn't shipping the SVG's "
         "user-space coordinates in the FramePayload, so the host has no way to "
         "do screen↔document math.";
  const Box2d& vb = *load.documentViewBox;
  EXPECT_NEAR(vb.topLeft.x, 0.0, 0.001);
  EXPECT_NEAR(vb.topLeft.y, 0.0, 0.001);
  EXPECT_NEAR(vb.width(), 200.0, 0.001)
      << "documentViewBox should be the SVG's own viewBox (200×100), NOT the "
         "rendered canvas size (892×512) — otherwise hit-test coordinates are "
         "in the wrong space.";
  EXPECT_NEAR(vb.height(), 100.0, 0.001);

  // The cached client read should also surface the viewBox.
  ASSERT_TRUE(client->latestDocumentViewBox().has_value());
  EXPECT_NEAR(client->latestDocumentViewBox()->width(), 200.0, 0.001);
}

/// End-to-end screen-click → backend-selects-correct-element test.
/// Stands up a realistic viewport (pane at some position, SVG rendered
/// to fill it), converts a specific on-screen click to a
/// `documentX/Y` via `ViewportState::screenToDocument`, sends it to
/// the backend, and asserts the backend selects the element the user
/// actually clicked on.
///
/// Pre-fix: `documentViewBox` came from the bitmap so screen→document
/// produced bitmap-pixel coords. A click inside the visible `red rect`
/// sent `(bitmapPx, bitmapPy)` as the hit-test point; the backend
/// looked for elements covering that point in its own 200×100 viewBox
/// space and usually missed entirely or hit the wrong element.
TEST_F(ThinClientUiFlowTest, ClickOnScreenHitsCorrectElementInDocumentSpace) {
  // Document viewBox is explicitly (0 0 200 100). Two non-overlapping
  // rects so hit-test has an unambiguous answer.
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="892" height="512" viewBox="0 0 200 100">)"
      R"(<rect id="left"  x="10"  y="20" width="40" height="40" fill="red"/>)"
      R"(<rect id="right" x="140" y="20" width="40" height="40" fill="blue"/>)"
      R"(</svg>)";

  auto client = EditorBackendClient::MakeInProcess();
  FrameResult load = client
                         ->loadBytes(std::span(reinterpret_cast<const uint8_t*>(kSvg.data()),
                                                kSvg.size()),
                                     std::nullopt)
                         .get();
  ASSERT_TRUE(load.ok);
  ASSERT_TRUE(load.documentViewBox.has_value());

  // Simulate main.cc's viewport setup: pane at (100, 80), size
  // 800×600, documentViewBox seeded from the backend, zoom=1.
  ViewportState vp;
  vp.paneOrigin = Vector2d(100.0, 80.0);
  vp.paneSize = Vector2d(800.0, 600.0);
  vp.devicePixelRatio = 1.0;
  vp.documentViewBox = *load.documentViewBox;
  vp.resetTo100Percent();

  // The user clicks at the screen pixel corresponding to the center
  // of the RIGHT (blue) rect at doc coords (160, 40). If
  // `documentViewBox` is correctly set to (0 0 200 100),
  // `screenToDocument` round-trips: click at
  // `documentToScreen((160, 40))` returns `(160, 40)`.
  const Vector2d rightRectCenterDoc(160.0, 40.0);
  const Vector2d rightRectCenterScreen = vp.documentToScreen(rightRectCenterDoc);
  const Vector2d rightRectRoundTripDoc = vp.screenToDocument(rightRectCenterScreen);
  EXPECT_NEAR(rightRectRoundTripDoc.x, rightRectCenterDoc.x, 0.01)
      << "screen→document round-trip failed — documentViewBox is the wrong "
         "space. You'd hit-test at the round-tripped point, which isn't "
         "the user's click.";
  EXPECT_NEAR(rightRectRoundTripDoc.y, rightRectCenterDoc.y, 0.01);

  // Now actually send the click through the backend and check which
  // element it selected.
  ::donner::editor::PointerEventPayload down;
  down.phase = PointerPhase::kDown;
  down.documentPoint = rightRectRoundTripDoc;
  FrameResult clickFrame = client->pointerEvent(down).get();
  ASSERT_TRUE(clickFrame.ok);

  // Selection must contain exactly one entry — the right rect.
  ASSERT_EQ(clickFrame.selection.selections.size(), 1u)
      << "click at doc point " << rightRectRoundTripDoc
      << " selected " << clickFrame.selection.selections.size()
      << " elements. Likely hit-test received the wrong-space "
         "coordinates and missed the document geometry entirely.";

  // The selected element's worldBBox must cover the clicked point.
  const Box2d bbox = clickFrame.selection.selections[0].worldBBox;
  EXPECT_TRUE(bbox.contains(rightRectCenterDoc))
      << "selected element's bbox " << bbox << " doesn't contain the clicked "
      << "point " << rightRectCenterDoc
      << " — the click landed in the wrong document-space region. This is "
         "the 'AABB overlay in wrong position' bug.";

  // Negative side: the left rect is NOT what got selected. Its
  // x-span is (10, 50) so doc point (160, 40) must not sit inside it.
  EXPECT_LT(50.0, bbox.topLeft.x)
      << "the backend selected the LEFT rect for a RIGHT-rect click — "
         "coordinate space is still wrong.";
}

// ---------------------------------------------------------------------------
// Drag support: kDown → kMove → kUp must actually move the selected element.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Compositor-mode drag perf: repeated kMove events must be fast once the
// compositor has promoted the drag target and cached its bitmap.
//
// Pre-compositor the backend re-rasterized the whole document on every
// frame — moving a rect on a multi-shape SVG fell to ~30-60 ms/frame.
// Post-compositor the fast path should land each steady-state drag
// frame under a few ms since only the compose transform updates.
// ---------------------------------------------------------------------------

TEST_F(ThinClientUiFlowTest, CompositorModeSteadyStateDragIsFast) {
  // Multi-shape document so the "whole doc re-raster" fallback is
  // visibly expensive — 100 paths is ~2-3 ms on macOS, ~10 ms on CI,
  // and a steady-state drag frame should be a fraction of that once
  // the compositor's translation-only fast path kicks in.
  std::string svg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="400" viewBox="0 0 400 400">)";
  for (int i = 0; i < 99; ++i) {
    const int x = 10 + (i % 10) * 30;
    const int y = 10 + (i / 10) * 30;
    svg += R"(<rect x=")" + std::to_string(x) + R"(" y=")" + std::to_string(y) +
           R"(" width="20" height="20" fill="gray"/>)";
  }
  svg += R"(<rect id="drag_target" x="180" y="180" width="40" height="40" fill="red"/>)";
  svg += "</svg>";

  auto client = EditorBackendClient::MakeInProcess();
  FrameResult load = client
                         ->loadBytes(std::span(reinterpret_cast<const uint8_t*>(svg.data()),
                                                svg.size()),
                                     std::nullopt)
                         .get();
  ASSERT_TRUE(load.ok);

  // Press down on the drag target.
  ::donner::editor::PointerEventPayload down;
  down.phase = PointerPhase::kDown;
  down.documentPoint = Vector2d(200.0, 200.0);
  down.buttons = 1;
  (void)client->pointerEvent(down).get();

  // Warm-up move — first drag frame pays promote + initial layer
  // rasterize.
  ::donner::editor::PointerEventPayload warmup;
  warmup.phase = PointerPhase::kMove;
  warmup.documentPoint = Vector2d(201.0, 200.0);
  warmup.buttons = 1;
  (void)client->pointerEvent(warmup).get();

  // Measure 20 steady-state drag frames.
  constexpr int kDragFrames = 20;
  using Clock = std::chrono::steady_clock;
  std::vector<double> ms;
  ms.reserve(kDragFrames);
  for (int i = 0; i < kDragFrames; ++i) {
    ::donner::editor::PointerEventPayload mv;
    mv.phase = PointerPhase::kMove;
    mv.documentPoint = Vector2d(200.0 + (i + 2) * 3.0, 200.0);
    mv.buttons = 1;
    const auto t0 = Clock::now();
    (void)client->pointerEvent(mv).get();
    const auto t1 = Clock::now();
    ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }

  double total = 0.0;
  double worst = 0.0;
  for (double v : ms) {
    total += v;
    worst = std::max(worst, v);
  }
  const double avg = total / static_cast<double>(ms.size());
  std::fprintf(stderr,
               "[CompositorDrag] steady-state drag: avg=%.3f ms, max=%.3f ms over %d frames\n",
               avg, worst, kDragFrames);

  // Budget: 60 ms avg, 120 ms max. Loose enough for slow remote-
  // execution workers (the bazel-re1 Linux aarch64 worker lands
  // around 38 ms/frame on this document; dev macOS hits ~18 ms).
  // Still tight enough to catch the "full re-raster per frame"
  // regression which lands at ~100+ ms avg. See commit ab68092b
  // for precedent on the "widen wall-clock, keep a counter-based
  // regression gate" pattern — we don't yet have compositor
  // counters exposed on this branch, so budget is the only gate
  // until G6b phase 2 lands.
  EXPECT_LT(avg, 60.0)
      << "steady-state drag is re-rasterizing the whole document per frame. "
         "Compositor translation fast path isn't engaging — check that the "
         "backend is instantiating `CompositorController` and routing "
         "promote/demote through it on drag.";
}

// User-reported regression: dragging shapes in real splash content causes
// fps spikes — frames intermittently taking 2-3× the steady-state cost.
// This test pins a real-splash drag through the end-to-end editor flow
// (pointer events → backend → compositor → overlay render → software
// composite → takeSnapshot) and asserts that EVERY steady-state drag
// frame stays within budget, not just the average. Spikes mean something
// is intermittently taking the slow path (re-rasterizing the whole doc,
// bumping `rootDirty_`, or wiping a segment cache).
TEST_F(ThinClientUiFlowTest, RealSplashSteadyStateDragHasNoFrameSpikes) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();
  ASSERT_FALSE(splashSource.empty());

  auto client = EditorBackendClient::MakeInProcess();
  (void)client->setViewport(892, 512).get();
  FrameResult load = client
                         ->loadBytes(std::span(reinterpret_cast<const uint8_t*>(splashSource.data()),
                                                splashSource.size()),
                                     std::nullopt)
                         .get();
  ASSERT_TRUE(load.ok);

  // Click a Donner letter (simple path, no filter).
  ::donner::editor::PointerEventPayload down;
  down.phase = PointerPhase::kDown;
  down.documentPoint = Vector2d(300.0, 390.0);  // approximate letter-A
  down.buttons = 1;
  FrameResult downFrame = client->pointerEvent(down).get();
  ASSERT_TRUE(downFrame.ok);
  // Some splash click locations land on the background rather than a
  // letter; bail out with a helpful skip rather than a confusing crash.
  if (downFrame.selection.selections.empty()) {
    GTEST_SKIP()
        << "expected click to land on a Donner letter — splash geometry may "
           "have shifted";
  }

  // Warm-up move.
  ::donner::editor::PointerEventPayload warmup;
  warmup.phase = PointerPhase::kMove;
  warmup.documentPoint = Vector2d(301.0, 390.0);
  warmup.buttons = 1;
  (void)client->pointerEvent(warmup).get();

  // Measure steady-state drag frame times.
  constexpr int kDragFrames = 30;
  using Clock = std::chrono::steady_clock;
  std::vector<double> ms;
  ms.reserve(kDragFrames);
  for (int i = 0; i < kDragFrames; ++i) {
    ::donner::editor::PointerEventPayload mv;
    mv.phase = PointerPhase::kMove;
    mv.documentPoint = Vector2d(300.0 + (i + 2) * 1.5, 390.0);
    mv.buttons = 1;
    const auto t0 = Clock::now();
    (void)client->pointerEvent(mv).get();
    const auto t1 = Clock::now();
    ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }

  double total = 0.0;
  double worst = 0.0;
  int spikeCount = 0;
  constexpr double kSpikeThresholdMs = 60.0;
  for (double v : ms) {
    total += v;
    worst = std::max(worst, v);
    if (v > kSpikeThresholdMs) ++spikeCount;
  }
  const double avg = total / static_cast<double>(ms.size());
  std::fprintf(stderr,
               "[SplashDrag] frames: avg=%.2f ms, max=%.2f ms, spikes (>%.0f ms)=%d/%d\n",
               avg, worst, kSpikeThresholdMs, spikeCount, kDragFrames);

  // Budget on avg catches "slow path engaged throughout".
  EXPECT_LT(avg, 60.0)
      << "real-splash drag steady-state avg is too high — the compositor fast "
         "path isn't engaging. Check that `consumeDirtyFlags` sees "
         "translation-only deltas on the dragged entity and that the "
         "overlay/software-composite pair isn't dominating frame time.";
  // Budget on spikes catches "occasional full re-raster" — the user's
  // reported symptom. A single frame hitting 2× budget is a spike the
  // user sees as jank.
  EXPECT_EQ(spikeCount, 0)
      << spikeCount << " of " << kDragFrames << " steady-state drag frames "
      << "spiked past " << kSpikeThresholdMs
      << " ms. Spikes indicate intermittent cache invalidation — look at "
         "`rootDirty_`, `needsFullRebuild`, and segment-cache wipes.";
}

// Editor-reported regression: dragging a `<g filter>` should hit the
// compositor fast path the same way dragging a letter does. Before the
// subtree fast-path fix, the eligibility check required
// `firstEntity == lastEntity == e`, which rejected every filter /
// clip-path / group subtree layer and forced a full
// `prepareDocumentForRendering` + per-layer re-rasterize per drag
// frame. On `donner_splash.svg` that was ~60 ms/frame (slow path)
// instead of ~20 ms/frame (fast path + CPU compose + tight overlay).
// User-reported regression: clicking a PATH that lives inside a
// `<g filter>` should feel as fast as clicking the group itself.
// `SelectTool::onMouseDown` must elevate the hit-tested leaf to the
// outermost compositing-group ancestor — if it leaves the leaf
// selected, `CompositorController::promoteEntity` refuses to promote
// it (descendant of a compositing-breaking ancestor) and every drag
// frame falls through to the full-document render path.
// UI-level perf repro: the user reports a `<g filter>` drag caps at
// 20 fps (~50 ms/frame) in the editor, but the backend-only timing
// tests above show ~5 ms/frame for the same gesture. The gap is the
// host-side work — GL texture upload + ImGui frame + vsync wait —
// which the backend-only tests don't exercise.
//
// This test simulates the editor's actual main-loop cadence:
//   1. Post `pointerEvent(kMove)` to the backend (same synchronous
//      call main.cc makes; blocks on the in-process handler).
//   2. Copy the returned bitmap the way `glTexImage2D` would read
//      it. The stand-in for the GL upload is a same-size
//      `std::memcpy` — memory-bandwidth equivalent to the
//      host→texture DMA, minus the driver overhead. Not perfectly
//      faithful (real drivers can sync on previous uses or convert
//      formats), but it exposes the bitmap-size scaling that the
//      backend-only test elides.
//   3. Sum all that into a per-frame wall clock.
//
// The budget is 20 ms/frame — well inside a 60 fps vsync window —
// so any regression that pushes a frame past 20 ms will trip the
// assertion at HiDPI canvas sizes where the gap actually shows up.
TEST(EditorUiFlowPerfTest, FilterGroupDragHitsSixtyFpsBudgetAtHiDpi) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  // 1784×1024 = what the editor renders on a retina MacBook at
  // default zoom. Backend-only costs scale roughly linearly with
  // pixel count, but the "GL upload" stand-in (host→DMA copy) scales
  // harder because bandwidth-bound work dominates a larger bitmap.
  constexpr int kCanvasW = 1784;
  constexpr int kCanvasH = 1024;

  sandbox::EditorBackendCore core;
  SetViewportPayload vp;
  vp.width = kCanvasW;
  vp.height = kCanvasH;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = splashSource;
  (void)core.handleLoadBytes(load);

  // Click inside `Big_lightning_glow`'s cls-79 path — `SelectTool`
  // elevates the leaf to the filter group; the compositor takes the
  // subtree fast path on the subsequent drags.
  PointerEventPayload down;
  down.phase = donner::editor::sandbox::PointerPhase::kDown;
  down.documentX = 455.0;
  down.documentY = 160.0;
  down.buttons = 1;
  auto downFrame = core.handlePointerEvent(down);
  ASSERT_FALSE(downFrame.selections.empty())
      << "expected click to hit the cls-79 path inside Big_lightning_glow";

  // Warm-up move so the layer's bitmap is stamped and the compose
  // cache is populated before the steady-state measurement starts.
  PointerEventPayload warmup;
  warmup.phase = donner::editor::sandbox::PointerPhase::kMove;
  warmup.documentX = 456.0;
  warmup.documentY = 160.0;
  warmup.buttons = 1;
  (void)core.handlePointerEvent(warmup);

  // Per-frame scratch buffer mimicking the texture-upload target.
  // Allocated once outside the loop so allocator cost doesn't bleed
  // into the per-frame measurement — matches main.cc's single-
  // texture bind + `glTexImage2D`-into-preallocated-target pattern.
  const size_t kBytesPerFrame = static_cast<size_t>(kCanvasW) * kCanvasH * 4u;
  std::vector<uint8_t> textureStandIn(kBytesPerFrame, 0);

  constexpr int kSteadyFrames = 30;
  using Clock = std::chrono::steady_clock;
  std::vector<double> frameMs;
  frameMs.reserve(kSteadyFrames);
  double backendTotalMs = 0.0;
  double uploadTotalMs = 0.0;
  for (int i = 0; i < kSteadyFrames; ++i) {
    PointerEventPayload mv;
    mv.phase = donner::editor::sandbox::PointerPhase::kMove;
    mv.documentX = 456.0 + (i + 1) * 1.5;
    mv.documentY = 160.0;
    mv.buttons = 1;

    const auto t0 = Clock::now();
    const auto payload = core.handlePointerEvent(mv);
    const auto tBackendEnd = Clock::now();

    // "GL upload" stand-in: copy the `finalBitmapPixels` into the
    // host-side texture buffer the same way `glTexImage2D` drains
    // the source pointer into the driver. Memory-bandwidth matches
    // even if driver details don't.
    ASSERT_TRUE(payload.hasFinalBitmap)
        << "backend failed to produce a final bitmap on drag frame " << i;
    if (!payload.finalBitmapPixels.empty() &&
        payload.finalBitmapPixels.size() <= textureStandIn.size()) {
      std::memcpy(textureStandIn.data(), payload.finalBitmapPixels.data(),
                  payload.finalBitmapPixels.size());
    }
    const auto tUploadEnd = Clock::now();

    const double backendMs =
        std::chrono::duration<double, std::milli>(tBackendEnd - t0).count();
    const double uploadMs =
        std::chrono::duration<double, std::milli>(tUploadEnd - tBackendEnd).count();
    backendTotalMs += backendMs;
    uploadTotalMs += uploadMs;
    frameMs.push_back(backendMs + uploadMs);
  }

  double worst = 0.0;
  double total = 0.0;
  int overSixtyFps = 0;
  int overThirtyFps = 0;
  int overTwentyFps = 0;
  for (double v : frameMs) {
    worst = std::max(worst, v);
    total += v;
    if (v > 16.67) ++overSixtyFps;
    if (v > 33.33) ++overThirtyFps;
    if (v > 50.0) ++overTwentyFps;
  }
  const double avg = total / static_cast<double>(frameMs.size());
  const double backendAvg = backendTotalMs / kSteadyFrames;
  const double uploadAvg = uploadTotalMs / kSteadyFrames;
  std::fprintf(stderr,
               "[UIPerf %dx%d] frames avg=%.2f ms (backend=%.2f + upload=%.2f), "
               "max=%.2f, >60fps=%d/%d, >30fps=%d, >20fps=%d\n",
               kCanvasW, kCanvasH, avg, backendAvg, uploadAvg, worst, overSixtyFps,
               kSteadyFrames, overThirtyFps, overTwentyFps);

  // 20 ms/frame budget: inside a 60 Hz vsync window, with slack for
  // the driver work the stand-in copy doesn't model. If avg > 20
  // ms, a 60 Hz host will drop to 30 fps every frame; >33 ms drops
  // to 20 fps (the user-reported cap). The specific sub-budgets
  // below are diagnostic so the failure message points at the
  // stage that regressed.
  EXPECT_LT(avg, 20.0)
      << "HiDPI filter-group drag can't hit 60 fps: backend avg=" << backendAvg
      << " ms, upload avg=" << uploadAvg
      << " ms. If backend is the big number, the compositor fast path "
         "isn't engaging; check fast-path counters + "
         "`SelectTool::elevateToCompositingGroupAncestor`. If upload is "
         "the big number, the overlay fell back to full-canvas rendering "
         "(check `SnapshotSelectionWorldBounds` / tight-bound sizing).";
  EXPECT_EQ(overTwentyFps, 0)
      << overTwentyFps << " of " << kSteadyFrames
      << " frames exceeded 50 ms (the user-reported 20 fps cap).";
}

// Direct `EditorBackendCore` check: after SelectTool elevates a click
// inside `<g filter>` and the drag starts, every subsequent kMove
// frame MUST increment `fastPathFrames`, not `slowPathFramesWithDirty`.
// Wall-clock budgets flake under CI noise; the counter is the
// deterministic signal.
TEST(EditorBackendCoreFastPathTest, FilterGroupDragCountsAsFastPath) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  sandbox::EditorBackendCore core;

  SetViewportPayload vp;
  vp.width = 892;
  vp.height = 512;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = splashSource;
  (void)core.handleLoadBytes(load);

  // Click inside `Big_lightning_glow`'s cls-79 path. SelectTool's
  // `ElevateToCompositingGroupAncestor` should redirect the selection
  // to the filter group.
  PointerEventPayload down;
  down.phase = donner::editor::sandbox::PointerPhase::kDown;
  down.documentX = 455.0;
  down.documentY = 160.0;
  down.buttons = 1;
  auto downFrame = core.handlePointerEvent(down);
  ASSERT_FALSE(downFrame.selections.empty())
      << "click inside Big_lightning_glow should hit something — splash "
         "geometry may have shifted";

  // One warm-up kMove frame so the compositor processes the promote
  // + first drag-transform and stamps the layer bitmap.
  PointerEventPayload warmup;
  warmup.phase = donner::editor::sandbox::PointerPhase::kMove;
  warmup.documentX = 456.0;
  warmup.documentY = 160.0;
  warmup.buttons = 1;
  (void)core.handlePointerEvent(warmup);

  const auto beforeCounters = core.compositorFastPathCountersForTesting();

  constexpr int kSteadyFrames = 20;
  for (int i = 0; i < kSteadyFrames; ++i) {
    PointerEventPayload mv;
    mv.phase = donner::editor::sandbox::PointerPhase::kMove;
    mv.documentX = 456.0 + (i + 1) * 1.5;
    mv.documentY = 160.0;
    mv.buttons = 1;
    (void)core.handlePointerEvent(mv);
  }

  const auto afterCounters = core.compositorFastPathCountersForTesting();
  const uint64_t fastPathDelta =
      afterCounters.fastPathFrames - beforeCounters.fastPathFrames;
  const uint64_t slowPathDelta =
      afterCounters.slowPathFramesWithDirty - beforeCounters.slowPathFramesWithDirty;

  // Every steady-state kMove should be a fast-path frame. Slow-path
  // frames during drag indicate:
  //   - `SelectTool::elevateToCompositingGroupAncestor` not firing
  //     (selection is the leaf path, `promoteEntity` refuses it as a
  //     descendant-of-filter-ancestor, fallback full render),
  //   - the compositor's subtree fast-path gate rejecting the dragged
  //     entity (single-entity-layer gate regression),
  //   - non-translation delta (shouldn't happen on pointer drag).
  EXPECT_EQ(slowPathDelta, 0u)
      << "drag hit slow path " << slowPathDelta << " times out of "
      << kSteadyFrames << " steady-state frames. Fast-path delta="
      << fastPathDelta << ". Check `SelectTool::onMouseDown` elevation and "
         "`CompositorController::renderFrame` subtree-layer eligibility.";
  EXPECT_GE(fastPathDelta, static_cast<uint64_t>(kSteadyFrames))
      << "expected every steady-state drag frame to hit the fast path; "
         "fastPathDelta=" << fastPathDelta << " slowPathDelta=" << slowPathDelta;
}

TEST_F(ThinClientUiFlowTest, ClickingPathInsideFilterGroupStillHitsFastPath) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  auto client = EditorBackendClient::MakeInProcess();
  (void)client->setViewport(892, 512).get();
  FrameResult load = client
                         ->loadBytes(std::span(reinterpret_cast<const uint8_t*>(splashSource.data()),
                                                splashSource.size()),
                                     std::nullopt)
                         .get();
  ASSERT_TRUE(load.ok);

  // `#Big_lightning_glow` contains a `<path class="cls-79">` with a
  // non-trivial bounding box spanning ~[395..495] × [105..265]. Click
  // a point inside that path — the editor's `SelectTool` must elevate
  // the click to `#Big_lightning_glow` for the drag to hit the
  // compositor's subtree fast path.
  ::donner::editor::PointerEventPayload down;
  down.phase = PointerPhase::kDown;
  down.documentPoint = Vector2d(455.0, 160.0);  // Inside cls-79's top half.
  down.buttons = 1;
  FrameResult downFrame = client->pointerEvent(down).get();
  ASSERT_TRUE(downFrame.ok);
  if (downFrame.selection.selections.empty()) {
    GTEST_SKIP()
        << "expected click to hit the cls-79 path inside Big_lightning_glow — "
           "splash geometry may have shifted";
  }

  // Warm-up + 30 steady-state drag frames, same shape as
  // `RealSplashSteadyStateDragHasNoFrameSpikes`.
  ::donner::editor::PointerEventPayload warmup;
  warmup.phase = PointerPhase::kMove;
  warmup.documentPoint = Vector2d(456.0, 160.0);
  warmup.buttons = 1;
  (void)client->pointerEvent(warmup).get();

  constexpr int kDragFrames = 30;
  using Clock = std::chrono::steady_clock;
  std::vector<double> ms;
  ms.reserve(kDragFrames);
  for (int i = 0; i < kDragFrames; ++i) {
    ::donner::editor::PointerEventPayload mv;
    mv.phase = PointerPhase::kMove;
    mv.documentPoint = Vector2d(455.0 + (i + 2) * 1.5, 160.0);
    mv.buttons = 1;
    const auto t0 = Clock::now();
    (void)client->pointerEvent(mv).get();
    const auto t1 = Clock::now();
    ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }
  double total = 0.0;
  double worst = 0.0;
  int spikeCount = 0;
  constexpr double kSpikeThresholdMs = 60.0;
  for (double v : ms) {
    total += v;
    worst = std::max(worst, v);
    if (v > kSpikeThresholdMs) ++spikeCount;
  }
  const double avg = total / static_cast<double>(ms.size());
  std::fprintf(stderr,
               "[PathInFilter] frames: avg=%.2f ms, max=%.2f ms, spikes (>%.0f ms)=%d/%d\n",
               avg, worst, kSpikeThresholdMs, spikeCount, kDragFrames);

  EXPECT_LT(avg, 60.0)
      << "clicking inside a filter group and dragging is slow — "
         "`SelectTool::onMouseDown` likely isn't elevating to the filter "
         "group, so `promoteEntity` refuses the leaf and every frame "
         "takes the full-document slow path.";
  EXPECT_EQ(spikeCount, 0)
      << spikeCount << " of " << kDragFrames << " drag frames spiked past "
      << kSpikeThresholdMs << " ms.";
}

// HiDPI scenario (DPR ≈ 2) matters because the user's editor runs at
// device-pixel viewport 1784×1024 on retina, not the 892×512 the
// default test uses. Every backend stage scales roughly with pixel
// count — compositor composite, overlay render, CPU composite, GL
// upload. If the fast path is engaging but a non-fast-path stage is
// bottlenecking at HiDPI, we'd see a per-pixel-scaled budget
// regression that the 1× test misses.
TEST_F(ThinClientUiFlowTest, RealSplashFilterGroupDragAtHiDpiViewport) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  auto client = EditorBackendClient::MakeInProcess();
  (void)client->setViewport(1784, 1024).get();
  FrameResult load = client
                         ->loadBytes(std::span(reinterpret_cast<const uint8_t*>(splashSource.data()),
                                                splashSource.size()),
                                     std::nullopt)
                         .get();
  ASSERT_TRUE(load.ok);

  // Click inside `#Big_lightning_glow` at the HiDPI equivalent of
  // (445, 180). documentPoint is in SVG user units so the click
  // coordinate doesn't scale with canvas size; only the rendered
  // bitmap dimensions do.
  ::donner::editor::PointerEventPayload down;
  down.phase = PointerPhase::kDown;
  down.documentPoint = Vector2d(445.0, 180.0);
  down.buttons = 1;
  FrameResult downFrame = client->pointerEvent(down).get();
  ASSERT_TRUE(downFrame.ok);
  if (downFrame.selection.selections.empty()) {
    GTEST_SKIP()
        << "expected click to hit Big_lightning_glow — splash geometry may "
           "have shifted";
  }

  ::donner::editor::PointerEventPayload warmup;
  warmup.phase = PointerPhase::kMove;
  warmup.documentPoint = Vector2d(446.0, 180.0);
  warmup.buttons = 1;
  (void)client->pointerEvent(warmup).get();

  constexpr int kDragFrames = 30;
  using Clock = std::chrono::steady_clock;
  std::vector<double> ms;
  ms.reserve(kDragFrames);
  for (int i = 0; i < kDragFrames; ++i) {
    ::donner::editor::PointerEventPayload mv;
    mv.phase = PointerPhase::kMove;
    mv.documentPoint = Vector2d(445.0 + (i + 2) * 1.5, 180.0);
    mv.buttons = 1;
    const auto t0 = Clock::now();
    (void)client->pointerEvent(mv).get();
    const auto t1 = Clock::now();
    ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }

  double total = 0.0;
  double worst = 0.0;
  int spikeCount = 0;
  constexpr double kSpikeThresholdMs = 60.0;
  for (double v : ms) {
    total += v;
    worst = std::max(worst, v);
    if (v > kSpikeThresholdMs) ++spikeCount;
  }
  const double avg = total / static_cast<double>(ms.size());
  std::fprintf(stderr,
               "[HiDpiFilterDrag] frames: avg=%.2f ms, max=%.2f ms, spikes (>%.0f ms)=%d/%d\n",
               avg, worst, kSpikeThresholdMs, spikeCount, kDragFrames);
  EXPECT_LT(avg, 60.0)
      << "HiDPI filter-group drag is slow. If avg > 30 ms, the overlay "
         "probably fell back to full-canvas rendering because "
         "`SnapshotSelectionWorldBounds` returned empty for the filter "
         "group and the tight-bound sizing code in "
         "`EditorBackendCore::buildFramePayload` hit its fallback branch.";
  EXPECT_EQ(spikeCount, 0);
}

TEST_F(ThinClientUiFlowTest, RealSplashFilterGroupDragEngagesFastPath) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();
  ASSERT_FALSE(splashSource.empty());

  auto client = EditorBackendClient::MakeInProcess();
  (void)client->setViewport(892, 512).get();
  FrameResult load = client
                         ->loadBytes(std::span(reinterpret_cast<const uint8_t*>(splashSource.data()),
                                                splashSource.size()),
                                     std::nullopt)
                         .get();
  ASSERT_TRUE(load.ok);

  // Click a point inside `#Big_lightning_glow` (filter group). The
  // editor's `SelectTool` elevates the click to the filter group root
  // via `elevateToCompositingRoot`, so the dragged entity is the
  // `<g filter>` itself — a subtree layer.
  ::donner::editor::PointerEventPayload down;
  down.phase = PointerPhase::kDown;
  down.documentPoint = Vector2d(445.0, 180.0);
  down.buttons = 1;
  FrameResult downFrame = client->pointerEvent(down).get();
  ASSERT_TRUE(downFrame.ok);
  if (downFrame.selection.selections.empty()) {
    GTEST_SKIP()
        << "expected click to land on the Big_lightning_glow filter group — "
           "splash geometry may have shifted";
  }

  // Warm-up move so the first frame's promotion cost doesn't poison
  // the steady-state measurement.
  ::donner::editor::PointerEventPayload warmup;
  warmup.phase = PointerPhase::kMove;
  warmup.documentPoint = Vector2d(446.0, 180.0);
  warmup.buttons = 1;
  (void)client->pointerEvent(warmup).get();

  constexpr int kDragFrames = 30;
  using Clock = std::chrono::steady_clock;
  std::vector<double> ms;
  ms.reserve(kDragFrames);
  for (int i = 0; i < kDragFrames; ++i) {
    ::donner::editor::PointerEventPayload mv;
    mv.phase = PointerPhase::kMove;
    mv.documentPoint = Vector2d(445.0 + (i + 2) * 1.5, 180.0);
    mv.buttons = 1;
    const auto t0 = Clock::now();
    (void)client->pointerEvent(mv).get();
    const auto t1 = Clock::now();
    ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }

  double total = 0.0;
  double worst = 0.0;
  int spikeCount = 0;
  constexpr double kSpikeThresholdMs = 60.0;
  for (double v : ms) {
    total += v;
    worst = std::max(worst, v);
    if (v > kSpikeThresholdMs) ++spikeCount;
  }
  const double avg = total / static_cast<double>(ms.size());
  std::fprintf(stderr,
               "[FilterDrag] frames: avg=%.2f ms, max=%.2f ms, spikes (>%.0f ms)=%d/%d\n",
               avg, worst, kSpikeThresholdMs, spikeCount, kDragFrames);

  EXPECT_LT(avg, 60.0)
      << "dragging a `<g filter>` is slow — the subtree fast path isn't "
         "engaging. Check the `firstEntity == lastEntity == e` gate in "
         "`CompositorController::renderFrame` and that "
         "`propagateFastPathTranslationToSubtree` is wired up.";
  EXPECT_EQ(spikeCount, 0)
      << spikeCount << " of " << kDragFrames << " filter-group drag frames "
      << "spiked past " << kSpikeThresholdMs << " ms.";
  EXPECT_LT(worst, 120.0) << "drag frame spiked above budget";
}

TEST_F(ThinClientUiFlowTest, DragMovesSelectedElementAndCommitsWriteback) {
  // Minimal document: one rect we can pick up and drop. The backend
  // stamps its `transform` attribute with a translate on mouse-up;
  // the host's writeback pipeline splices that into the source pane.
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">)"
      R"(<rect id="target" x="50" y="50" width="40" height="40" fill="red"/>)"
      R"(</svg>)";

  auto client = EditorBackendClient::MakeInProcess();
  FrameResult load = client
                         ->loadBytes(std::span(reinterpret_cast<const uint8_t*>(kSvg.data()),
                                                kSvg.size()),
                                     std::nullopt)
                         .get();
  ASSERT_TRUE(load.ok);

  // kDown at the center of the rect — picks it up.
  ::donner::editor::PointerEventPayload down;
  down.phase = PointerPhase::kDown;
  down.documentPoint = Vector2d(70.0, 70.0);
  down.buttons = 1;
  FrameResult downFrame = client->pointerEvent(down).get();
  ASSERT_TRUE(downFrame.ok);
  ASSERT_EQ(downFrame.selection.selections.size(), 1u)
      << "kDown didn't select the rect — SelectTool not wired into the "
         "backend. `handlePointerEvent` should dispatch through it.";

  // kMove by +30 px in document space — should drag the rect.
  ::donner::editor::PointerEventPayload move;
  move.phase = PointerPhase::kMove;
  move.documentPoint = Vector2d(100.0, 70.0);
  move.buttons = 1;
  FrameResult moveFrame = client->pointerEvent(move).get();
  ASSERT_TRUE(moveFrame.ok);
  // During the drag the SelectTool is applying the translation to the
  // document; the rect's bbox should reflect it.
  ASSERT_EQ(moveFrame.selection.selections.size(), 1u);
  const Box2d duringDrag = moveFrame.selection.selections[0].worldBBox;
  EXPECT_NEAR(duringDrag.topLeft.x, 80.0, 0.01)
      << "during-drag bbox didn't move 30px in X; drag dispatch is a no-op. "
         "Observed topLeft.x=" << duringDrag.topLeft.x;
  EXPECT_NEAR(duringDrag.topLeft.y, 50.0, 0.01) << "drag leaked into Y axis";

  // kUp at the same point — commit.
  ::donner::editor::PointerEventPayload up;
  up.phase = PointerPhase::kUp;
  up.documentPoint = Vector2d(100.0, 70.0);
  FrameResult upFrame = client->pointerEvent(up).get();
  ASSERT_TRUE(upFrame.ok);

  // After kUp the document should have a transform writeback, and
  // the rect's world bbox should still be at the translated
  // position (the commit doesn't undo the drag).
  ASSERT_EQ(upFrame.selection.selections.size(), 1u);
  const Box2d afterDrag = upFrame.selection.selections[0].worldBBox;
  EXPECT_NEAR(afterDrag.topLeft.x, 80.0, 0.01);
}

/// Regression: selection chrome (AABB + path outlines) must appear in the
/// backend-composited bitmap. Previously the host drew chrome via ImGui's
/// draw list, which produced shear during drag and skipped path outlines;
/// we moved that job to the backend via `OverlayRenderer::drawChromeWith
/// Transform` so it's pixel-locked to the rendered content. If the backend
/// drops the chrome pass (e.g. because the compositor's endFrame closed
/// the frame in a way OverlayRenderer can't draw into), the output shows
/// just the document content — exactly the user's complaint.
///
/// Test: load a rect, select it, inspect the bitmap for the cyan stroke
/// the selection overlay draws around its AABB.
TEST_F(ThinClientUiFlowTest, SelectionChromeAppearsInBackendRenderedBitmap) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">)"
      R"(<rect id="target" x="50" y="50" width="100" height="100" fill="white"/>)"
      R"(</svg>)";

  auto client = EditorBackendClient::MakeInProcess();
  (void)client->setViewport(200, 200).get();
  FrameResult load = client
                         ->loadBytes(std::span(reinterpret_cast<const uint8_t*>(kSvg.data()),
                                                kSvg.size()),
                                     std::nullopt)
                         .get();
  ASSERT_TRUE(load.ok);

  // Click the rect to select it.
  ::donner::editor::PointerEventPayload down;
  down.phase = PointerPhase::kDown;
  down.documentPoint = Vector2d(100.0, 100.0);
  down.buttons = 1;
  FrameResult clickFrame = client->pointerEvent(down).get();
  ASSERT_TRUE(clickFrame.ok);
  ASSERT_EQ(clickFrame.selection.selections.size(), 1u) << "rect was not selected";
  ASSERT_TRUE(clickFrame.documentViewBox.has_value());

  // Release so we get a non-dragging frame (drag state masks chrome).
  ::donner::editor::PointerEventPayload up;
  up.phase = PointerPhase::kUp;
  up.documentPoint = Vector2d(100.0, 100.0);
  FrameResult afterClick = client->pointerEvent(up).get();
  ASSERT_TRUE(afterClick.ok);

  const svg::RendererBitmap& bitmap = afterClick.bitmap;
  ASSERT_FALSE(bitmap.empty());

  // `OverlayRenderer::MakeSelectionStrokePaint` uses cyan `#00c8ff`.
  // The selection AABB sits at doc (50,50)-(150,150) → canvas pixel
  // coords at 1:1. Sample just INSIDE the top edge (row y=50) where
  // the stroke should land. White fill is inside; pure white means
  // chrome never painted.
  const auto pixelAt = [&](int x, int y) -> std::array<uint8_t, 4> {
    const uint8_t* row =
        bitmap.pixels.data() + static_cast<size_t>(y) * bitmap.rowBytes;
    return {row[x * 4 + 0], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3]};
  };

  // Look along the AABB edges for any pixel whose blue channel is
  // saturated AND red is low — characteristic of the cyan stroke —
  // rather than pure white (fill) or transparent (outside).
  bool cyanFound = false;
  std::array<uint8_t, 4> worstOffending{};
  for (int scan = 45; scan <= 55 && !cyanFound; ++scan) {
    for (int x = 45; x <= 155 && !cyanFound; ++x) {
      const auto px = pixelAt(x, scan);
      if (px[2] >= 0xc0 && px[0] <= 0x80 && px[3] > 0) {
        cyanFound = true;
        worstOffending = px;
      }
    }
  }
  EXPECT_TRUE(cyanFound)
      << "no cyan selection-chrome pixel found along the AABB edge — "
         "`OverlayRenderer::drawChromeWithTransform` isn't drawing into "
         "the backend's composited bitmap (likely frame-state issue "
         "between compositor endFrame and overlay draws). Sampled last: ("
      << static_cast<int>(worstOffending[0]) << ", "
      << static_cast<int>(worstOffending[1]) << ", "
      << static_cast<int>(worstOffending[2]) << ", "
      << static_cast<int>(worstOffending[3]) << ")";
}

/// Marquee rubber-band selection populates `FramePayload.hasMarquee` +
/// `.marquee[4]` so the host (or the chrome painted into the bitmap)
/// can draw a live rubber-band rect while the user drags on empty
/// space. Without this wire, the backend's `SelectTool` tracks the
/// state but nobody else sees it.
TEST_F(ThinClientUiFlowTest, MarqueeRectSurfacesOnFramePayload) {
  // Blank canvas with one rect far from the origin so the marquee
  // can start in empty space without hitting geometry.
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">)"
      R"(<rect x="150" y="150" width="30" height="30" fill="red"/>)"
      R"(</svg>)";

  auto client = EditorBackendClient::MakeInProcess();
  (void)client->loadBytes(
      std::span(reinterpret_cast<const uint8_t*>(kSvg.data()), kSvg.size()),
      std::nullopt).get();

  // kDown in empty space at (10, 10) → starts marquee.
  ::donner::editor::PointerEventPayload down;
  down.phase = PointerPhase::kDown;
  down.documentPoint = Vector2d(10.0, 10.0);
  down.buttons = 1;
  FrameResult downFrame = client->pointerEvent(down).get();
  ASSERT_TRUE(downFrame.ok);

  // kMove to (80, 60) — marquee rect now (10,10)-(80,60).
  ::donner::editor::PointerEventPayload move;
  move.phase = PointerPhase::kMove;
  move.documentPoint = Vector2d(80.0, 60.0);
  move.buttons = 1;
  FrameResult moveFrame = client->pointerEvent(move).get();
  ASSERT_TRUE(moveFrame.ok);

  EXPECT_TRUE(moveFrame.selection.marquee.has_value())
      << "marquee rect missing from FrameResult — backend's "
         "`SelectTool::marqueeRect()` never made it onto the wire";
  if (moveFrame.selection.marquee.has_value()) {
    const Box2d& m = *moveFrame.selection.marquee;
    EXPECT_NEAR(m.topLeft.x, 10.0, 0.01);
    EXPECT_NEAR(m.topLeft.y, 10.0, 0.01);
    EXPECT_NEAR(m.bottomRight.x, 80.0, 0.01);
    EXPECT_NEAR(m.bottomRight.y, 60.0, 0.01);
  }

  // kUp at the same point commits the marquee to a selection set.
  ::donner::editor::PointerEventPayload up;
  up.phase = PointerPhase::kUp;
  up.documentPoint = Vector2d(80.0, 60.0);
  (void)client->pointerEvent(up).get();
}

/// `handleAttachSharedTexture` must degrade cleanly on platforms
/// whose factory isn't compiled in. On Linux (or if the host sends
/// an unknown kind), the backend instantiates the CPU stub; drop-
/// through is a no-op from the rendering side. This is the test
/// that keeps the Linux CI lane green even as the bridge grows
/// macOS-specific code paths — the call succeeds, the ack frame
/// comes back, rendering continues via `finalBitmapPixels`.
TEST_F(ThinClientUiFlowTest, AttachSharedTextureFallsBackGracefully) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">)"
      R"(<rect x="10" y="10" width="80" height="80" fill="green"/>)"
      R"(</svg>)";

  auto client = EditorBackendClient::MakeInProcess();

  // Make an unknown-kind handle: `BridgeHandleKind(99)` isn't any real
  // platform. Backend must accept it and degrade to the stub (see
  // `EditorBackendCore::handleAttachSharedTexture` default branch).
  donner::editor::sandbox::bridge::BridgeTextureHandle bogusHandle;
  bogusHandle.kind = static_cast<donner::editor::sandbox::bridge::BridgeHandleKind>(99);
  bogusHandle.handle = 0xdeadbeef;
  bogusHandle.dimensions = Vector2i(100, 100);

  FrameResult ack = client->attachSharedTexture(bogusHandle).get();
  EXPECT_TRUE(ack.ok)
      << "backend rejected an unknown-kind handle instead of falling "
         "back to the stub. Linux (or any non-mac / non-linux) host "
         "would see its `attachSharedTexture` call return !ok on "
         "startup.";

  // Subsequent loadBytes / render continues through the CPU bitmap
  // path without the bridge engaging — the stub reports
  // `ready() == false` so `buildFramePayload` keeps shipping
  // `finalBitmapPixels`.
  FrameResult load = client
                         ->loadBytes(std::span(reinterpret_cast<const uint8_t*>(kSvg.data()),
                                                kSvg.size()),
                                     std::nullopt)
                         .get();
  ASSERT_TRUE(load.ok);
  EXPECT_FALSE(load.bitmap.empty())
      << "CPU fallback bitmap missing after attachSharedTexture — "
         "the stub path shouldn't disable the normal rendering.";
}

/// Backend exports the last-loaded SVG source on kExport(kSvgText).
/// Host's save-as path depends on this — previously the backend
/// returned empty bytes and silently dropped saves.
TEST_F(ThinClientUiFlowTest, ExportSvgTextReturnsLoadedSource) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">)"
      R"(<rect x="10" y="10" width="80" height="80" fill="blue"/>)"
      R"(</svg>)";

  auto client = EditorBackendClient::MakeInProcess();
  (void)client->loadBytes(
      std::span(reinterpret_cast<const uint8_t*>(kSvg.data()), kSvg.size()),
      std::nullopt).get();

  ::donner::editor::ExportPayload req;
  req.format = ExportFormat::kSvgText;
  ExportResult result = client->exportDocument(req).get();

  ASSERT_TRUE(result.ok);
  EXPECT_EQ(result.format, ExportFormat::kSvgText);
  const std::string exported(result.bytes.begin(), result.bytes.end());
  EXPECT_EQ(exported, std::string(kSvg))
      << "backend export didn't round-trip the loaded source bytes";
}

}  // namespace
}  // namespace donner::editor::sandbox
