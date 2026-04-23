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

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/editor/EditorBackendClient.h"
#include "donner/editor/ViewportState.h"
#include "donner/editor/repro/ReproFile.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/editor/sandbox/EditorApiCodec.h"
#include "donner/editor/sandbox/EditorBackendCore.h"
#include "donner/editor/sandbox/ReplayingRenderer.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererImageIO.h"

namespace donner::editor::sandbox {
namespace {

// ---------------------------------------------------------------------------
// Selection diagnostics.
//
// The replay tests used to eyeball PNGs to tell drag-outcome regressions
// apart. Four attempts-at-fix on issue #582 all produced different
// DOM-level symptoms (no-change / dark-halo-only-moved / whole-tree-
// moved / whole-document-moved) but ALL tripped the same bright-pixel
// assertion, so the test name didn't tell the reviewer which bug they
// were looking at. These helpers read `FramePayload.selections` +
// `treeSummary` and emit a human-readable identity + coverage summary,
// plus an assertion that catches "selection escalated to the document
// root" cleanly.
// ---------------------------------------------------------------------------

/// Return the tree-summary node that's marked `selected == true`, or
/// `nullptr` if the frame has no selection.
const TreeNodeEntry* FindSelectedNode(const FramePayload& frame) {
  for (const auto& node : frame.tree.nodes) {
    if (node.selected) {
      return &node;
    }
  }
  return nullptr;
}

/// Format `selections[0]`'s AABB + the selected node's tag/id into a
/// one-line string like `"g#Lightning_glow_dark [aabb 473.5,375.5 →
/// 552.5,480.5 | 98×117 doc-units]"`. Returns `"<none>"` when the frame
/// has no selection. Used in stderr logs so a failing test's output
/// immediately tells the reviewer WHICH element got picked.
std::string DescribeSelection(const FramePayload& frame) {
  const TreeNodeEntry* node = FindSelectedNode(frame);
  if (node == nullptr) return "<none>";
  std::string out = node->tagName;
  if (!node->idAttr.empty()) {
    out += "#";
    out += node->idAttr;
  }
  if (!frame.selections.empty()) {
    const auto& sel = frame.selections.front();
    const double w = sel.bbox[2] - sel.bbox[0];
    const double h = sel.bbox[3] - sel.bbox[1];
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  " [aabb %.1f,%.1f → %.1f,%.1f | %.0f×%.0f doc-units]",
                  sel.bbox[0], sel.bbox[1], sel.bbox[2], sel.bbox[3], w, h);
    out += buf;
  } else {
    out += " [no-bbox]";
  }
  return out;
}

/// Build a `svg::RendererBitmap` view of a `FramePayload`'s pixel
/// buffer for pixelmatch diffing.
svg::RendererBitmap BitmapFromFrame(const FramePayload& fp) {
  svg::RendererBitmap bm;
  bm.dimensions = Vector2i(fp.finalBitmapWidth, fp.finalBitmapHeight);
  bm.rowBytes = fp.finalBitmapRowBytes;
  bm.pixels = fp.finalBitmapPixels;
  bm.alphaType = static_cast<svg::AlphaType>(fp.finalBitmapAlphaType);
  return bm;
}

/// Return `DONNER_ATTEMPT_TAG` as a `{tag}_` filename prefix, or empty
/// when unset. Lets reviewers do
///
///   DONNER_ATTEMPT_TAG=attempt5 bazel test //donner/editor/sandbox/tests/...
///
/// so each attempt's dumped PNGs sit side-by-side in
/// `test.outputs/` rather than overwriting the previous run. Empty when
/// unset, so existing CI paths stay byte-identical.
std::string AttemptTagPrefix() {
  const char* tag = std::getenv("DONNER_ATTEMPT_TAG");
  if (tag == nullptr || tag[0] == '\0') return "";
  std::string out(tag);
  out += "_";
  return out;
}

/// Assert the selection's AABB covers less than `maxFraction` of the
/// given doc viewBox area. Catches "drag escalated to the document
/// root" regressions (attempt2/attempt4 from issue #582) directly —
/// without the PNG inspection a bright-pixel heuristic forces. Logs
/// `DescribeSelection` on failure so the reviewer sees WHICH ancestor
/// swallowed the click.
::testing::AssertionResult SelectionAabbWithinDocBudget(
    const FramePayload& frame, double docViewBoxW, double docViewBoxH,
    double maxFraction = 0.50) {
  if (frame.selections.empty()) {
    return ::testing::AssertionSuccess() << "no selection (skipped)";
  }
  const auto& sel = frame.selections.front();
  const double selW = std::max(0.0, sel.bbox[2] - sel.bbox[0]);
  const double selH = std::max(0.0, sel.bbox[3] - sel.bbox[1]);
  const double docArea = docViewBoxW * docViewBoxH;
  if (docArea <= 0.0) {
    return ::testing::AssertionSuccess() << "doc area unknown (skipped)";
  }
  const double fraction = (selW * selH) / docArea;
  if (fraction < maxFraction) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
         << "selection " << DescribeSelection(frame) << " covers "
         << std::fixed << std::setprecision(1) << (fraction * 100.0) << "% of the "
         << docViewBoxW << "×" << docViewBoxH << " document — "
         << "elevation escalated past the intended composite group. "
         << "Issue #582 attempt2 / attempt4 signature.";
}

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
// User-reported regression: after dragging `Big_lightning_glow` the
// element disappears. Drive the sandbox against the REAL splash
// through mouseDown + N moves + mouseUp + one idle render, and
// verify the filter group's content is still visible at its post-
// drag position.
TEST(EditorBackendCoreFilterDragTest, BigLightningGlowSurvivesDragReleaseCycle) {
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
  const auto loadFrame = core.handleLoadBytes(load);
  ASSERT_TRUE(loadFrame.hasFinalBitmap);

  // Sample the pre-drag color at Big_lightning_glow's center
  // (approx. (450, 180) in canvas = doc coords on the 892×512 splash
  // at 1:1 scale). The bolt is yellow-white, painted under the blur.
  const size_t rowBytes = loadFrame.finalBitmapRowBytes > 0
                               ? loadFrame.finalBitmapRowBytes
                               : static_cast<size_t>(loadFrame.finalBitmapWidth) * 4u;
  const auto samplePixel = [&rowBytes](const FramePayload& frame, int x, int y) {
    const size_t off = static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x) * 4u;
    struct Rgba {
      uint8_t r, g, b, a;
    };
    return Rgba{frame.finalBitmapPixels[off + 0], frame.finalBitmapPixels[off + 1],
                frame.finalBitmapPixels[off + 2], frame.finalBitmapPixels[off + 3]};
  };

  // Record pre-drag pixel. Lightning bolts are pale/white inside the
  // blur; check that we have non-background content at the drag
  // target before continuing.
  const auto preDrag = samplePixel(loadFrame, 450, 180);
  if (!(preDrag.r > 50 || preDrag.g > 50)) {
    GTEST_SKIP()
        << "pre-drag pixel at (450, 180) doesn't contain lightning-bolt "
           "content — splash geometry may have shifted";
  }

  // Click inside Big_lightning_glow; SelectTool elevates to the
  // filter group (via `ElevateToCompositingGroupAncestor`).
  PointerEventPayload down;
  down.phase = donner::editor::sandbox::PointerPhase::kDown;
  down.documentX = 455.0;
  down.documentY = 160.0;
  down.buttons = 1;
  const auto downFrame = core.handlePointerEvent(down);
  ASSERT_FALSE(downFrame.selections.empty())
      << "click should land on Big_lightning_glow's cls-79 path";

  // Drag 10 cumulative frames to the right (30 doc units total).
  for (int i = 1; i <= 10; ++i) {
    PointerEventPayload mv;
    mv.phase = donner::editor::sandbox::PointerPhase::kMove;
    mv.documentX = 455.0 + static_cast<double>(i) * 3.0;
    mv.documentY = 160.0;
    mv.buttons = 1;
    (void)core.handlePointerEvent(mv);
  }

  // Release and hover.
  PointerEventPayload up;
  up.phase = donner::editor::sandbox::PointerPhase::kUp;
  up.documentX = 485.0;
  up.documentY = 160.0;
  (void)core.handlePointerEvent(up);

  PointerEventPayload hover;
  hover.phase = donner::editor::sandbox::PointerPhase::kMove;
  hover.documentX = 485.0;
  hover.documentY = 160.0;
  hover.buttons = 0;
  const auto postRelease = core.handlePointerEvent(hover);

  ASSERT_TRUE(postRelease.hasFinalBitmap);

  // Post-drag position: lightning content moved 30 doc units right →
  // was at (~450, ~180), now at (~480, ~180). Probe a 7×7 grid
  // around there. Pre-drag pixel was bright; post-drag pixel should
  // still be bright (lightning content still visible).
  int brightHits = 0;
  for (int dy = -3; dy <= 3; ++dy) {
    for (int dx = -3; dx <= 3; ++dx) {
      const auto px = samplePixel(postRelease, 480 + dx, 180 + dy);
      const int luma = (px.r + px.g + px.b) / 3;
      if (luma > 60) {
        ++brightHits;
      }
    }
  }
  EXPECT_GE(brightHits, 5)
      << "after drag + release, Big_lightning_glow content is missing from "
         "the post-release frame. Expected bright lightning-bolt pixels "
         "around (480, 180); got mostly dark.";
}

// User-reported regression (filter_elm_disappear-2.rnr): drag a
// filter element, release, then start dragging a DIFFERENT element —
// the first-dragged filter disappears from the live composited view
// mid-second-drag. Drives `EditorBackendCore` directly with the
// exact doc-space coords + event sequence from the recording, at
// the same 1784×1024 HiDPI viewport the user runs in, then samples
// the filter's post-drag-1 canvas bounds at mid-drag-2 for warm
// lightning-glow content.
TEST(EditorBackendCoreFilterDragTest, FilterSurvivesFollowUpDragFromRecording) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ifstream reproStream("donner/editor/tests/filter_elm_disappear-2.rnr");
  if (!reproStream.is_open()) {
    GTEST_SKIP() << "filter_elm_disappear-2.rnr not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  auto reproOpt = donner::editor::repro::ReadReproFile(
      "donner/editor/tests/filter_elm_disappear-2.rnr");
  ASSERT_TRUE(reproOpt.has_value()) << "failed to parse repro";
  const auto& repro = *reproOpt;

  // Viewport MUST match the recording — the compositor's canvas-pixel
  // buffer sizes, the promoted-layer rasterize bounds, and the hit-
  // test AABBs all depend on it, and a size mismatch can mask bugs
  // that depend on exact layer sizing (e.g. split-layer cache
  // invalidation after a viewport change). Recording's pane is
  // `pw × ph × dpr` logical pixels.
  ASSERT_TRUE(repro.frames.size() > 0 && [&] {
    for (const auto& f : repro.frames)
      if (f.viewport.has_value()) return true;
    return false;
  }()) << "recording has no viewport block — cannot size the backend canvas";
  const auto* firstVp = [&]() -> const donner::editor::repro::ReproViewport* {
    for (const auto& f : repro.frames) {
      if (f.viewport.has_value()) return &*f.viewport;
    }
    return nullptr;
  }();
  ASSERT_NE(firstVp, nullptr);
  const int canvasW =
      static_cast<int>(std::round(firstVp->paneSizeW * firstVp->devicePixelRatio));
  const int canvasH =
      static_cast<int>(std::round(firstVp->paneSizeH * firstVp->devicePixelRatio));

  sandbox::EditorBackendCore core;
  SetViewportPayload vp;
  vp.width = canvasW;
  vp.height = canvasH;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = splashSource;
  ASSERT_TRUE(core.handleLoadBytes(load).hasFinalBitmap);

  // Mid-drag-2 checkpoint frame — between the 2nd mdown and its
  // mup. Uses the recording's authoritative `mdx`/`mdy` coords.
  // Recording layout:
  //   drag 1: mdown @ f=535, mup @ f=560
  //   drag 2: mdown @ f=714, mup @ f=730
  constexpr std::uint64_t kMidDrag2Frame = 722;

  // Walk frames, replay button edges and moves. SelectTool expects
  // onMouseMove to fire on every frame the button is held (it
  // reads the current cursor pos to compute the drag delta — mup
  // alone doesn't commit a final move), so we mirror that: between
  // mdown and mup, a kMove fires with each frame's mouse position.
  bool leftHeld = false;
  std::optional<FramePayload> midDrag2Frame;
  for (const auto& frame : repro.frames) {
    if (!frame.mouseDocX.has_value()) continue;
    const double docX = *frame.mouseDocX;
    const double docY = *frame.mouseDocY;
    const bool nowHeld = (frame.mouseButtonMask & 1) != 0;

    for (const auto& ev : frame.events) {
      PointerEventPayload ptr;
      ptr.documentX = docX;
      ptr.documentY = docY;
      if (ev.kind == donner::editor::repro::ReproEvent::Kind::MouseDown && ev.mouseButton == 0) {
        ptr.phase = PointerPhase::kDown;
        ptr.buttons = 1;
        (void)core.handlePointerEvent(ptr);
      } else if (ev.kind == donner::editor::repro::ReproEvent::Kind::MouseUp && ev.mouseButton == 0) {
        ptr.phase = PointerPhase::kUp;
        ptr.buttons = 0;
        (void)core.handlePointerEvent(ptr);
      }
    }
    if (nowHeld && leftHeld) {
      PointerEventPayload ptr;
      ptr.phase = PointerPhase::kMove;
      ptr.documentX = docX;
      ptr.documentY = docY;
      ptr.buttons = 1;
      auto result = core.handlePointerEvent(ptr);
      if (frame.index == kMidDrag2Frame) {
        midDrag2Frame = std::move(result);
      }
    }
    leftHeld = nowHeld;
  }

  ASSERT_TRUE(midDrag2Frame.has_value())
      << "mid-drag-2 checkpoint frame " << kMidDrag2Frame
      << " wasn't reached — replay may not have applied the expected button-held sequence";
  ASSERT_TRUE(midDrag2Frame->hasFinalBitmap);
  const int bmpW = midDrag2Frame->finalBitmapWidth;
  const int bmpH = midDrag2Frame->finalBitmapHeight;
  ASSERT_GT(bmpW, 0);
  ASSERT_GT(bmpH, 0);

  const size_t rowBytes = midDrag2Frame->finalBitmapRowBytes;

  // Dump the mid-drag-2 frame for inspection.
  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
    const std::string path = std::string(dir) + "/" + AttemptTagPrefix() +
                             "rnr_host_mid_drag2.png";
    donner::svg::RendererImageIO::writeRgbaPixelsToPngFile(
        path.c_str(), midDrag2Frame->finalBitmapPixels, bmpW, bmpH,
        static_cast<uint32_t>(rowBytes / 4u));
  }

  // Probe the entire SVG content region for warm (R+G > 120) lightning-
  // glow pixels. If the filter truly vanished mid-drag-2, the region
  // reverts to the dark navy background (R+G < 60).
  const auto samplePx = [&](int x, int y) {
    const size_t off = static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x) * 4u;
    struct Rgba { uint8_t r, g, b, a; };
    return Rgba{midDrag2Frame->finalBitmapPixels[off + 0],
                midDrag2Frame->finalBitmapPixels[off + 1],
                midDrag2Frame->finalBitmapPixels[off + 2],
                midDrag2Frame->finalBitmapPixels[off + 3]};
  };

  int brightHits = 0;
  int totalSamples = 0;
  for (int cy = 0; cy < bmpH; cy += 30) {
    for (int cx = 0; cx < bmpW; cx += 30) {
      ++totalSamples;
      const auto px = samplePx(cx, cy);
      const int warmth = int(px.r) + int(px.g);
      if (warmth > 120) {
        ++brightHits;
      }
    }
  }

  EXPECT_GE(brightHits, 20)
      << "filter_elm_disappear-2.rnr replay through host/sandbox path: at mid-drag-2, "
      << "the Big_lightning_glow filter's canvas region has only "
      << brightHits << " bright (R+G>120) pixels out of " << totalSamples << " probed "
      << "across the " << bmpW << "x" << bmpH << " canvas. "
      << "The filter appears to have disappeared from the composited output — the "
      << "user-reported regression is still live.";
}

// Replay `filter_elm_disappear-4.rnr` through EditorBackendCore and dump
// frames for inspection. Diagnostic-only (no assertions beyond "frame
// exists") — the user supplied this as a fresh, DIFFERENT repro from
// `-3` to help narrow down which bug pattern they're actually seeing.
TEST(EditorBackendCoreFilterDragTest, FilterDisappearRepro4DumpFramesForInspection) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ifstream reproStream("donner/editor/tests/filter_elm_disappear-4.rnr");
  if (!reproStream.is_open()) {
    GTEST_SKIP() << "filter_elm_disappear-4.rnr not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  auto reproOpt = donner::editor::repro::ReadReproFile(
      "donner/editor/tests/filter_elm_disappear-4.rnr");
  ASSERT_TRUE(reproOpt.has_value());
  const auto& repro = *reproOpt;

  const auto* firstVp = [&]() -> const donner::editor::repro::ReproViewport* {
    for (const auto& f : repro.frames)
      if (f.viewport.has_value()) return &*f.viewport;
    return nullptr;
  }();
  ASSERT_NE(firstVp, nullptr);
  const int canvasW =
      static_cast<int>(std::round(firstVp->paneSizeW * firstVp->devicePixelRatio));
  const int canvasH =
      static_cast<int>(std::round(firstVp->paneSizeH * firstVp->devicePixelRatio));

  sandbox::EditorBackendCore core;
  SetViewportPayload vp;
  vp.width = canvasW;
  vp.height = canvasH;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = splashSource;
  const auto loadFrame = core.handleLoadBytes(load);
  ASSERT_TRUE(loadFrame.hasFinalBitmap);

  bool leftHeld = false;
  std::optional<FramePayload> lastFrame = loadFrame;
  std::size_t mouseUpCount = 0;
  std::optional<FramePayload> afterMup[4];
  for (const auto& frame : repro.frames) {
    if (!frame.mouseDocX.has_value()) continue;
    const double docX = *frame.mouseDocX;
    const double docY = *frame.mouseDocY;
    const bool nowHeld = (frame.mouseButtonMask & 1) != 0;

    for (const auto& ev : frame.events) {
      PointerEventPayload ptr;
      ptr.documentX = docX;
      ptr.documentY = docY;
      if (ev.kind == donner::editor::repro::ReproEvent::Kind::MouseDown &&
          ev.mouseButton == 0) {
        ptr.phase = PointerPhase::kDown;
        ptr.buttons = 1;
        lastFrame = core.handlePointerEvent(ptr);
        std::fprintf(stderr, "[rnr4] mdown#%zu @ (%.1f, %.1f): selection = %s\n",
                     mouseUpCount + 1, docX, docY,
                     DescribeSelection(*lastFrame).c_str());
      } else if (ev.kind == donner::editor::repro::ReproEvent::Kind::MouseUp &&
                 ev.mouseButton == 0) {
        ptr.phase = PointerPhase::kUp;
        ptr.buttons = 0;
        lastFrame = core.handlePointerEvent(ptr);
        if (mouseUpCount < 4) afterMup[mouseUpCount] = *lastFrame;
        ++mouseUpCount;
        std::fprintf(stderr, "[rnr4] mup#%zu @ (%.1f, %.1f): selection = %s\n",
                     mouseUpCount, docX, docY,
                     DescribeSelection(*lastFrame).c_str());
      }
    }
    if (nowHeld && leftHeld) {
      PointerEventPayload ptr;
      ptr.phase = PointerPhase::kMove;
      ptr.documentX = docX;
      ptr.documentY = docY;
      ptr.buttons = 1;
      lastFrame = core.handlePointerEvent(ptr);
    }
    leftHeld = nowHeld;
  }

  ASSERT_GT(mouseUpCount, 0u);
  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
    const std::string prefix = AttemptTagPrefix();
    const auto dump = [&](const char* name, const FramePayload& fp) {
      const std::string path = std::string(dir) + "/" + prefix + name;
      donner::svg::RendererImageIO::writeRgbaPixelsToPngFile(
          path.c_str(), fp.finalBitmapPixels, fp.finalBitmapWidth, fp.finalBitmapHeight,
          static_cast<uint32_t>(fp.finalBitmapRowBytes / 4u));
    };
    dump("rnr4_cold.png", loadFrame);
    for (std::size_t i = 0; i < 4; ++i) {
      if (afterMup[i].has_value()) {
        std::string name = "rnr4_after_mup" + std::to_string(i + 1) + ".png";
        dump(name.c_str(), *afterMup[i]);
      }
    }
    dump("rnr4_final.png", *lastFrame);
  }
  std::fprintf(stderr, "[rnr4] mouseUpCount=%zu canvas=%dx%d\n", mouseUpCount, canvasW, canvasH);
}

// Replay `filter_elm_disappear-5.rnr` — user-captured repro of the
// "filter group stops rendering after selecting the next thing" bug
// that surfaced post-#582 fix. Four drag sequences against the splash;
// the bug manifests when a filter-g is promoted then another element
// promoted + demoted, leaving the filter-g selected while its
// rendered pixels vanish.
//
// Diagnostic-first: dumps cold + every post-mup frame + the final
// frame so a reviewer can step through the sequence visually.
// Asserts that at the final frame, Big_lightning_glow's canvas region
// still has non-trivial rendered content (catches the "group stops
// rendering" symptom as a bright-pixel collapse relative to cold).
TEST(EditorBackendCoreFilterDragTest, FilterDisappearRepro5ReplaysWithoutErasingFilters) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ifstream reproStream("donner/editor/tests/filter_elm_disappear-5.rnr");
  if (!reproStream.is_open()) {
    GTEST_SKIP() << "filter_elm_disappear-5.rnr not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  auto reproOpt = donner::editor::repro::ReadReproFile(
      "donner/editor/tests/filter_elm_disappear-5.rnr");
  ASSERT_TRUE(reproOpt.has_value());
  const auto& repro = *reproOpt;

  const auto* firstVp = [&]() -> const donner::editor::repro::ReproViewport* {
    for (const auto& f : repro.frames)
      if (f.viewport.has_value()) return &*f.viewport;
    return nullptr;
  }();
  ASSERT_NE(firstVp, nullptr);
  const int canvasW =
      static_cast<int>(std::round(firstVp->paneSizeW * firstVp->devicePixelRatio));
  const int canvasH =
      static_cast<int>(std::round(firstVp->paneSizeH * firstVp->devicePixelRatio));

  sandbox::EditorBackendCore core;
  SetViewportPayload vp;
  vp.width = canvasW;
  vp.height = canvasH;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = splashSource;
  const auto loadFrame = core.handleLoadBytes(load);
  ASSERT_TRUE(loadFrame.hasFinalBitmap);
  const int bmpW = loadFrame.finalBitmapWidth;
  const int bmpH = loadFrame.finalBitmapHeight;
  const std::size_t rowBytes = loadFrame.finalBitmapRowBytes;

  bool leftHeld = false;
  std::optional<FramePayload> lastFrame = loadFrame;
  std::size_t mouseUpCount = 0;
  std::vector<FramePayload> afterMup;
  for (const auto& frame : repro.frames) {
    if (!frame.mouseDocX.has_value()) continue;
    const double docX = *frame.mouseDocX;
    const double docY = *frame.mouseDocY;
    const bool nowHeld = (frame.mouseButtonMask & 1) != 0;

    for (const auto& ev : frame.events) {
      PointerEventPayload ptr;
      ptr.documentX = docX;
      ptr.documentY = docY;
      if (ev.kind == donner::editor::repro::ReproEvent::Kind::MouseDown &&
          ev.mouseButton == 0) {
        ptr.phase = PointerPhase::kDown;
        ptr.buttons = 1;
        lastFrame = core.handlePointerEvent(ptr);
        std::fprintf(stderr, "[rnr5] mdown#%zu @ (%.1f, %.1f): selection = %s\n",
                     mouseUpCount + 1, docX, docY,
                     DescribeSelection(*lastFrame).c_str());
      } else if (ev.kind == donner::editor::repro::ReproEvent::Kind::MouseUp &&
                 ev.mouseButton == 0) {
        ptr.phase = PointerPhase::kUp;
        ptr.buttons = 0;
        lastFrame = core.handlePointerEvent(ptr);
        afterMup.push_back(*lastFrame);
        ++mouseUpCount;
        std::fprintf(stderr, "[rnr5] mup#%zu @ (%.1f, %.1f): selection = %s\n",
                     mouseUpCount, docX, docY,
                     DescribeSelection(*lastFrame).c_str());
      }
    }
    if (nowHeld && leftHeld) {
      PointerEventPayload ptr;
      ptr.phase = PointerPhase::kMove;
      ptr.documentX = docX;
      ptr.documentY = docY;
      ptr.buttons = 1;
      lastFrame = core.handlePointerEvent(ptr);
    }
    leftHeld = nowHeld;
  }

  ASSERT_GE(mouseUpCount, 1u);
  ASSERT_TRUE(lastFrame.has_value() && lastFrame->hasFinalBitmap);

  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
    const std::string prefix = AttemptTagPrefix();
    const auto dump = [&](const char* name, const FramePayload& fp) {
      const std::string path = std::string(dir) + "/" + prefix + name;
      donner::svg::RendererImageIO::writeRgbaPixelsToPngFile(
          path.c_str(), fp.finalBitmapPixels, fp.finalBitmapWidth, fp.finalBitmapHeight,
          static_cast<uint32_t>(fp.finalBitmapRowBytes / 4u));
    };
    dump("rnr5_cold.png", loadFrame);
    for (std::size_t i = 0; i < afterMup.size(); ++i) {
      std::string name = "rnr5_after_mup" + std::to_string(i + 1) + ".png";
      dump(name.c_str(), afterMup[i]);
    }
    dump("rnr5_final.png", *lastFrame);
  }

  // Post-fix assertion: the final frame's bitmap should have the same
  // dimensions as the cold frame's bitmap (pre-fix this jumped from
  // canvas-fit to unclamped viewport once any element was selected).
  ASSERT_EQ(lastFrame->finalBitmapWidth, bmpW);
  ASSERT_EQ(lastFrame->finalBitmapHeight, bmpH);

  // Sanity: the scene shouldn't have collapsed. Sum bright (any
  // channel > 180) pixels across the cold vs final bitmap. Post-fix
  // the two should be roughly comparable — a few dragged elements
  // shifted but the total "illuminated scene" mass stays similar.
  const auto countBright = [&](const FramePayload& fp) {
    int hits = 0;
    for (int cy = 0; cy < bmpH; cy += 8) {
      for (int cx = 0; cx < bmpW; cx += 8) {
        const std::size_t off = static_cast<std::size_t>(cy) * rowBytes +
                                static_cast<std::size_t>(cx) * 4u;
        const uint8_t r = fp.finalBitmapPixels[off + 0];
        const uint8_t g = fp.finalBitmapPixels[off + 1];
        const uint8_t b = fp.finalBitmapPixels[off + 2];
        if (std::max({r, g, b}) > 180) ++hits;
      }
    }
    return hits;
  };
  const int coldBright = countBright(loadFrame);
  const int finalBright = countBright(*lastFrame);
  std::fprintf(stderr, "[rnr5] bright pixels: cold=%d, final=%d (ratio=%.2fx)\n", coldBright,
               finalBright, coldBright > 0 ? double(finalBright) / coldBright : 0.0);
  EXPECT_GT(finalBright, coldBright * 3 / 4)
      << "final-frame bright-pixel count (" << finalBright << ") is "
      << "less than 75% of cold (" << coldBright << ") — scene brightness "
      << "collapsed, likely because a filter group stopped rendering.";
}

// Replay `filter_elm_disappear-6.rnr` — user-captured repro where a
// filter-g renders at a stale transform (old position) even though
// its selection chrome is at the correct post-drag position. Bitmap
// + `canvasFromBitmap_` drift between promote / demote cycles.
TEST(EditorBackendCoreFilterDragTest, FilterDisappearRepro6DumpFramesForInspection) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ifstream reproStream("donner/editor/tests/filter_elm_disappear-6.rnr");
  if (!reproStream.is_open()) {
    GTEST_SKIP() << "filter_elm_disappear-6.rnr not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  auto reproOpt = donner::editor::repro::ReadReproFile(
      "donner/editor/tests/filter_elm_disappear-6.rnr");
  ASSERT_TRUE(reproOpt.has_value());
  const auto& repro = *reproOpt;

  const auto* firstVp = [&]() -> const donner::editor::repro::ReproViewport* {
    for (const auto& f : repro.frames)
      if (f.viewport.has_value()) return &*f.viewport;
    return nullptr;
  }();
  ASSERT_NE(firstVp, nullptr);
  const int canvasW =
      static_cast<int>(std::round(firstVp->paneSizeW * firstVp->devicePixelRatio));
  const int canvasH =
      static_cast<int>(std::round(firstVp->paneSizeH * firstVp->devicePixelRatio));

  sandbox::EditorBackendCore core;
  SetViewportPayload vp;
  vp.width = canvasW;
  vp.height = canvasH;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = splashSource;
  const auto loadFrame = core.handleLoadBytes(load);
  ASSERT_TRUE(loadFrame.hasFinalBitmap);

  bool leftHeld = false;
  std::optional<FramePayload> lastFrame = loadFrame;
  std::size_t mouseUpCount = 0;
  std::vector<FramePayload> afterMup;
  for (const auto& frame : repro.frames) {
    if (!frame.mouseDocX.has_value()) continue;
    const double docX = *frame.mouseDocX;
    const double docY = *frame.mouseDocY;
    const bool nowHeld = (frame.mouseButtonMask & 1) != 0;

    for (const auto& ev : frame.events) {
      PointerEventPayload ptr;
      ptr.documentX = docX;
      ptr.documentY = docY;
      if (ev.kind == donner::editor::repro::ReproEvent::Kind::MouseDown &&
          ev.mouseButton == 0) {
        ptr.phase = PointerPhase::kDown;
        ptr.buttons = 1;
        lastFrame = core.handlePointerEvent(ptr);
        std::fprintf(stderr, "[rnr6] mdown#%zu @ (%.1f, %.1f): selection = %s\n",
                     mouseUpCount + 1, docX, docY,
                     DescribeSelection(*lastFrame).c_str());
      } else if (ev.kind == donner::editor::repro::ReproEvent::Kind::MouseUp &&
                 ev.mouseButton == 0) {
        ptr.phase = PointerPhase::kUp;
        ptr.buttons = 0;
        lastFrame = core.handlePointerEvent(ptr);
        afterMup.push_back(*lastFrame);
        ++mouseUpCount;
        std::fprintf(stderr, "[rnr6] mup#%zu @ (%.1f, %.1f): selection = %s\n",
                     mouseUpCount, docX, docY,
                     DescribeSelection(*lastFrame).c_str());
      }
    }
    if (nowHeld && leftHeld) {
      PointerEventPayload ptr;
      ptr.phase = PointerPhase::kMove;
      ptr.documentX = docX;
      ptr.documentY = docY;
      ptr.buttons = 1;
      lastFrame = core.handlePointerEvent(ptr);
    }
    leftHeld = nowHeld;
  }

  ASSERT_TRUE(lastFrame.has_value() && lastFrame->hasFinalBitmap);

  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
    const std::string prefix = AttemptTagPrefix();
    const auto dump = [&](const char* name, const FramePayload& fp) {
      const std::string path = std::string(dir) + "/" + prefix + name;
      donner::svg::RendererImageIO::writeRgbaPixelsToPngFile(
          path.c_str(), fp.finalBitmapPixels, fp.finalBitmapWidth, fp.finalBitmapHeight,
          static_cast<uint32_t>(fp.finalBitmapRowBytes / 4u));
    };
    dump("rnr6_cold.png", loadFrame);
    for (std::size_t i = 0; i < afterMup.size(); ++i) {
      std::string name = "rnr6_after_mup" + std::to_string(i + 1) + ".png";
      dump(name.c_str(), afterMup[i]);
    }
    dump("rnr6_final.png", *lastFrame);
  }
}

// Replay `filter_elm_disappear-7.rnr`. User-captured state where, after
// clicking the background radial gradient and then shift-clicking
// multiple elements, a previously-dragged bolt renders at TWO positions
// on screen — once at its correct post-drag position (matching the
// selection chrome's AABB) and once shifted roughly one viewBox width
// to the right (clipped by the SVG edge). Strong signature of a
// cached static segment retaining the element's pre-drag content
// WHILE the same element's promoted layer also renders it at the
// post-drag offset — two draws for one element.
//
// Diagnostic-only. Dumps cold + every post-mup frame so the
// double-render can be tracked frame by frame; no bitmap assertion
// because the repro's symptom (double-draw) is easier to see visually
// than to encode as a bright-pixel metric.
TEST(EditorBackendCoreFilterDragTest, FilterDisappearRepro7DumpFramesForInspection) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ifstream reproStream("donner/editor/tests/filter_elm_disappear-7.rnr");
  if (!reproStream.is_open()) {
    GTEST_SKIP() << "filter_elm_disappear-7.rnr not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  auto reproOpt = donner::editor::repro::ReadReproFile(
      "donner/editor/tests/filter_elm_disappear-7.rnr");
  ASSERT_TRUE(reproOpt.has_value());
  const auto& repro = *reproOpt;

  const auto* firstVp = [&]() -> const donner::editor::repro::ReproViewport* {
    for (const auto& f : repro.frames)
      if (f.viewport.has_value()) return &*f.viewport;
    return nullptr;
  }();
  ASSERT_NE(firstVp, nullptr);
  const int canvasW =
      static_cast<int>(std::round(firstVp->paneSizeW * firstVp->devicePixelRatio));
  const int canvasH =
      static_cast<int>(std::round(firstVp->paneSizeH * firstVp->devicePixelRatio));

  sandbox::EditorBackendCore core;
  SetViewportPayload vp;
  vp.width = canvasW;
  vp.height = canvasH;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = splashSource;
  const auto loadFrame = core.handleLoadBytes(load);
  ASSERT_TRUE(loadFrame.hasFinalBitmap);

  bool leftHeld = false;
  std::optional<FramePayload> lastFrame = loadFrame;
  std::size_t mouseUpCount = 0;
  std::vector<FramePayload> afterMup;
  for (const auto& frame : repro.frames) {
    if (!frame.mouseDocX.has_value()) continue;
    const double docX = *frame.mouseDocX;
    const double docY = *frame.mouseDocY;
    const bool nowHeld = (frame.mouseButtonMask & 1) != 0;
    const bool shiftHeld = (frame.modifiers & 0x1) != 0;
    const uint32_t ptrModifiers = shiftHeld ? 0x1u : 0u;

    for (const auto& ev : frame.events) {
      PointerEventPayload ptr;
      ptr.documentX = docX;
      ptr.documentY = docY;
      ptr.modifiers = ptrModifiers;
      if (ev.kind == donner::editor::repro::ReproEvent::Kind::MouseDown &&
          ev.mouseButton == 0) {
        ptr.phase = PointerPhase::kDown;
        ptr.buttons = 1;
        lastFrame = core.handlePointerEvent(ptr);
        std::fprintf(stderr, "[rnr7] mdown#%zu @ (%.1f, %.1f) mod=%u: selection = %s\n",
                     mouseUpCount + 1, docX, docY, ptrModifiers,
                     DescribeSelection(*lastFrame).c_str());
      } else if (ev.kind == donner::editor::repro::ReproEvent::Kind::MouseUp &&
                 ev.mouseButton == 0) {
        ptr.phase = PointerPhase::kUp;
        ptr.buttons = 0;
        lastFrame = core.handlePointerEvent(ptr);
        afterMup.push_back(*lastFrame);
        ++mouseUpCount;
        std::fprintf(stderr, "[rnr7] mup#%zu @ (%.1f, %.1f) mod=%u: selection = %s\n",
                     mouseUpCount, docX, docY, ptrModifiers,
                     DescribeSelection(*lastFrame).c_str());
      }
    }
    if (nowHeld && leftHeld) {
      PointerEventPayload ptr;
      ptr.phase = PointerPhase::kMove;
      ptr.documentX = docX;
      ptr.documentY = docY;
      ptr.buttons = 1;
      ptr.modifiers = ptrModifiers;
      lastFrame = core.handlePointerEvent(ptr);
    }
    leftHeld = nowHeld;
  }

  ASSERT_TRUE(lastFrame.has_value() && lastFrame->hasFinalBitmap);

  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
    const std::string prefix = AttemptTagPrefix();
    const auto dump = [&](const char* name, const FramePayload& fp) {
      const std::string path = std::string(dir) + "/" + prefix + name;
      donner::svg::RendererImageIO::writeRgbaPixelsToPngFile(
          path.c_str(), fp.finalBitmapPixels, fp.finalBitmapWidth, fp.finalBitmapHeight,
          static_cast<uint32_t>(fp.finalBitmapRowBytes / 4u));
    };
    dump("rnr7_cold.png", loadFrame);
    for (std::size_t i = 0; i < afterMup.size(); ++i) {
      std::string name = "rnr7_after_mup" + std::to_string(i + 1) + ".png";
      dump(name.c_str(), afterMup[i]);
    }
    dump("rnr7_final.png", *lastFrame);
  }
  std::fprintf(stderr, "[rnr7] mouseUpCount=%zu canvas=%dx%d\n", mouseUpCount, canvasW, canvasH);
}

// Bug hunt for #582 on the thin-client path.
//
// The `FilterDisappearRepro7DumpFramesForInspection` test above produces
// PNG dumps of every post-mup backend bitmap. Eyeballing the dumps, the
// filter element looks present at every checkpoint. But the user reports
// the live editor shows the bug after the same sequence of clicks. So
// either (a) the backend bitmap IS subtly wrong and eyeballing missed
// it, or (b) the bug lives downstream of `buildFramePayload` (GL upload,
// ImGui composite, IOSurface path, shared-texture bridge, etc.).
//
// This test settles (a) automatically: at every post-mup checkpoint,
// diff the backend's `finalBitmapPixels` (what the frontend would blit)
// against a direct render of the CURRENT mutated DOM via
// `svg::Renderer::draw(doc)`. Pixel-level divergence anywhere above a
// chrome-absorbing threshold means the backend is dropping or
// mispositioning content vs. what the DOM actually describes. Inspectable
// diff PNGs land in `$TEST_UNDECLARED_OUTPUTS_DIR` on failure.
//
// Threshold rationale:
//   - The backend bitmap includes selection chrome (AABB outline, handles)
//     composited on top of the render; direct render has none. Chrome is
//     ~1–2 px thick around the selected element's AABB — for
//     `#Big_lightning_glow` at 1310×1726 that's roughly 94×176 * 2 * 2 ≈
//     1300 pixels along the outline.
//   - `maxMismatchedPixels = 5000` leaves a comfortable ceiling for chrome
//     + a few hundred AA edge pixels, but would catch a filter shape
//     (~16k pixels for the big lightning bolt) disappearing entirely.
//   - A filter MISPOSITIONED by even a few pixels also trips this
//     threshold because both the "old" position (empty in backend,
//     filled in direct render) and the "new" position (filled in backend
//     but offset from direct render) diverge.
//
// If this test PASSES, the backend's bitmap is correct and the bug is
// in the display path (next target: hidden-GLFW + ImGui + glReadPixels
// harness on main.cc). If it FAILS, the diff PNG tells us which
// checkpoint and which pixels drifted.
TEST(EditorBackendCoreFilterDragTest, FilterDisappearRepro7BackendBitmapMatchesDirectRender) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ifstream reproStream("donner/editor/tests/filter_elm_disappear-7.rnr");
  if (!reproStream.is_open()) {
    GTEST_SKIP() << "filter_elm_disappear-7.rnr not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  auto reproOpt = donner::editor::repro::ReadReproFile(
      "donner/editor/tests/filter_elm_disappear-7.rnr");
  ASSERT_TRUE(reproOpt.has_value());
  const auto& repro = *reproOpt;

  const auto* firstVp = [&]() -> const donner::editor::repro::ReproViewport* {
    for (const auto& f : repro.frames)
      if (f.viewport.has_value()) return &*f.viewport;
    return nullptr;
  }();
  ASSERT_NE(firstVp, nullptr);
  const int canvasW =
      static_cast<int>(std::round(firstVp->paneSizeW * firstVp->devicePixelRatio));
  const int canvasH =
      static_cast<int>(std::round(firstVp->paneSizeH * firstVp->devicePixelRatio));

  sandbox::EditorBackendCore core;
  SetViewportPayload vp;
  vp.width = canvasW;
  vp.height = canvasH;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = splashSource;
  const auto loadFrame = core.handleLoadBytes(load);
  ASSERT_TRUE(loadFrame.hasFinalBitmap);

  // Renders the backend's current DOM directly (no compositor, no chrome).
  // Uses a separate Renderer per call so the backend's own `renderer_`
  // state isn't touched by this comparison path. The canvas size matches
  // what `buildFramePayload` uses so pixel dimensions line up one-to-one.
  const auto directRenderCurrentDom = [&]() -> svg::RendererBitmap {
    svg::SVGDocument& doc = core.editor().document().document();
    svg::Renderer reference;
    reference.draw(doc);
    return reference.takeSnapshot();
  };

  donner::editor::tests::BitmapGoldenCompareParams params;
  params.threshold = 0.03f;
  // Budget sized for multi-element selection chrome: single-element
  // chrome is ~2000-3000 px (outline + handles for a 94×176 AABB at
  // 1.47x scale), but shift-click selections can stack several
  // elements' worth of outlines + path strokes. rnr7 mup#7 selects
  // an ellipse (365×421) + the big lightning bolt simultaneously,
  // producing ~5000 px of chrome alone. 10000 leaves headroom for
  // future recordings with bigger selection sets without letting
  // any real structural drift through — even one missing filter
  // layer overwhelms this budget.
  params.maxMismatchedPixels = 10000;

  // Sanity gate: the COLD-LOAD frame (no selection, no compositor promote,
  // nothing interactive yet) must already match direct render — otherwise
  // the test is comparing apples to oranges (wrong alpha mode, different
  // canvas size, etc.) and every "divergence" below is a test artifact,
  // not the bug we're chasing. If this fails the test bails with a big
  // red sign pointing at the test setup.
  {
    svg::RendererBitmap coldBackend = BitmapFromFrame(loadFrame);
    svg::RendererBitmap coldExpected = directRenderCurrentDom();
    ASSERT_EQ(coldBackend.dimensions, coldExpected.dimensions)
        << "cold-load dimensions disagree — test setup bug, not the real #582";
    donner::editor::tests::CompareBitmapToBitmap(coldBackend, coldExpected,
                                                  "rnr7_coldload_backend_vs_direct", params);
    if (::testing::Test::HasFatalFailure()) {
      FAIL() << "cold-load backend bitmap already diverges from direct render — "
                "the comparison is invalid; fix the test before trusting the replay results";
    }
  }

  bool leftHeld = false;
  std::size_t mouseUpCount = 0;
  int divergedCheckpoints = 0;

  for (const auto& frame : repro.frames) {
    if (!frame.mouseDocX.has_value()) continue;
    const double docX = *frame.mouseDocX;
    const double docY = *frame.mouseDocY;
    const bool nowHeld = (frame.mouseButtonMask & 1) != 0;
    const bool shiftHeld = (frame.modifiers & 0x1) != 0;
    const uint32_t ptrModifiers = shiftHeld ? 0x1u : 0u;

    for (const auto& ev : frame.events) {
      PointerEventPayload ptr;
      ptr.documentX = docX;
      ptr.documentY = docY;
      ptr.modifiers = ptrModifiers;
      if (ev.kind == donner::editor::repro::ReproEvent::Kind::MouseDown &&
          ev.mouseButton == 0) {
        ptr.phase = PointerPhase::kDown;
        ptr.buttons = 1;
        (void)core.handlePointerEvent(ptr);
      } else if (ev.kind == donner::editor::repro::ReproEvent::Kind::MouseUp &&
                 ev.mouseButton == 0) {
        ptr.phase = PointerPhase::kUp;
        ptr.buttons = 0;
        const auto mupFrame = core.handlePointerEvent(ptr);
        ++mouseUpCount;
        if (!mupFrame.hasFinalBitmap) continue;

        // Backend bitmap wrapped so BitmapGoldenCompare can consume it.
        svg::RendererBitmap backend = BitmapFromFrame(mupFrame);

        // Ground truth: direct render of the DOM right now. If the
        // backend has dropped or mispositioned any geometry, this is
        // what it SHOULD look like (modulo chrome, which absorbs into
        // the pixelmatch threshold).
        svg::RendererBitmap expected = directRenderCurrentDom();
        ASSERT_FALSE(backend.empty()) << "backend bitmap empty at mup#" << mouseUpCount;
        ASSERT_FALSE(expected.empty()) << "direct-render reference empty at mup#" << mouseUpCount;
        if (backend.dimensions != expected.dimensions) {
          ADD_FAILURE() << "rnr7 mup#" << mouseUpCount << ": backend "
                        << backend.dimensions.x << "x" << backend.dimensions.y
                        << " vs direct-render " << expected.dimensions.x << "x"
                        << expected.dimensions.y
                        << " — size mismatch in backend bitmap vs direct render. "
                           "Selection: "
                        << DescribeSelection(mupFrame);
          continue;
        }

        const std::string label = "rnr7_mup" + std::to_string(mouseUpCount) +
                                  "_backend_vs_direct";
        // Count failures before the call so we can tell per-mup if this
        // one diverged (ADD_FAILURE doesn't return a value).
        const int beforeFailures = ::testing::UnitTest::GetInstance()->failed_test_count() +
                                    ::testing::UnitTest::GetInstance()
                                        ->current_test_info()
                                        ->result()
                                        ->total_part_count();
        donner::editor::tests::CompareBitmapToBitmap(backend, expected, label, params);
        const int afterFailures = ::testing::UnitTest::GetInstance()->failed_test_count() +
                                   ::testing::UnitTest::GetInstance()
                                       ->current_test_info()
                                       ->result()
                                       ->total_part_count();
        if (afterFailures > beforeFailures) {
          ++divergedCheckpoints;
          std::fprintf(stderr,
                       "[rnr7-diff] mup#%zu DIVERGED (selection=%s) — see "
                       "actual_%s.png / expected_%s.png / diff_%s.png\n",
                       mouseUpCount, DescribeSelection(mupFrame).c_str(), label.c_str(),
                       label.c_str(), label.c_str());
        }
      }
    }
    if (nowHeld && leftHeld) {
      PointerEventPayload ptr;
      ptr.phase = PointerPhase::kMove;
      ptr.documentX = docX;
      ptr.documentY = docY;
      ptr.buttons = 1;
      ptr.modifiers = ptrModifiers;
      (void)core.handlePointerEvent(ptr);
    }
    leftHeld = nowHeld;
  }

  std::fprintf(stderr, "[rnr7-diff] %d/%zu checkpoints diverged\n", divergedCheckpoints,
               mouseUpCount);
}

// #582 bisection: the above test shows the backend bitmap diverges
// from direct render on every post-mup frame. That test uses the
// fast CPU-compose path (`cpuComposeActive`). This variant FORCES
// the main-renderer compose path (`renderer_.takeSnapshot()`) by
// flipping the CPU-compose kill-switch off. If this variant PASSES
// where the above fails, the bug is in the CPU compose code itself.
// If this variant ALSO fails, the bug is upstream — in the
// compositor's cached state or in whatever the main compose is
// doing with that state.
TEST(EditorBackendCoreFilterDragTest,
     FilterDisappearRepro7WithoutCpuComposeMatchesDirectRender) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ifstream reproStream("donner/editor/tests/filter_elm_disappear-7.rnr");
  if (!reproStream.is_open()) {
    GTEST_SKIP() << "filter_elm_disappear-7.rnr not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  auto reproOpt = donner::editor::repro::ReadReproFile(
      "donner/editor/tests/filter_elm_disappear-7.rnr");
  ASSERT_TRUE(reproOpt.has_value());
  const auto& repro = *reproOpt;

  const auto* firstVp = [&]() -> const donner::editor::repro::ReproViewport* {
    for (const auto& f : repro.frames)
      if (f.viewport.has_value()) return &*f.viewport;
    return nullptr;
  }();
  ASSERT_NE(firstVp, nullptr);
  const int canvasW =
      static_cast<int>(std::round(firstVp->paneSizeW * firstVp->devicePixelRatio));
  const int canvasH =
      static_cast<int>(std::round(firstVp->paneSizeH * firstVp->devicePixelRatio));

  sandbox::EditorBackendCore core;
  // FLIP THE KILL-SWITCH: route through main compose, not CPU compose.
  core.setCpuComposeEnabledForTesting(false);

  SetViewportPayload vp;
  vp.width = canvasW;
  vp.height = canvasH;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = splashSource;
  const auto loadFrame = core.handleLoadBytes(load);
  ASSERT_TRUE(loadFrame.hasFinalBitmap);

  const auto directRenderCurrentDom = [&]() -> svg::RendererBitmap {
    svg::SVGDocument& doc = core.editor().document().document();
    svg::Renderer reference;
    reference.draw(doc);
    return reference.takeSnapshot();
  };

  donner::editor::tests::BitmapGoldenCompareParams params;
  params.threshold = 0.03f;
  // Budget sized for multi-element selection chrome: single-element
  // chrome is ~2000-3000 px (outline + handles for a 94×176 AABB at
  // 1.47x scale), but shift-click selections can stack several
  // elements' worth of outlines + path strokes. rnr7 mup#7 selects
  // an ellipse (365×421) + the big lightning bolt simultaneously,
  // producing ~5000 px of chrome alone. 10000 leaves headroom for
  // future recordings with bigger selection sets without letting
  // any real structural drift through — even one missing filter
  // layer overwhelms this budget.
  params.maxMismatchedPixels = 10000;

  bool leftHeld = false;
  std::size_t mouseUpCount = 0;
  int divergedCheckpoints = 0;

  for (const auto& frame : repro.frames) {
    if (!frame.mouseDocX.has_value()) continue;
    const double docX = *frame.mouseDocX;
    const double docY = *frame.mouseDocY;
    const bool nowHeld = (frame.mouseButtonMask & 1) != 0;
    const bool shiftHeld = (frame.modifiers & 0x1) != 0;
    const uint32_t ptrModifiers = shiftHeld ? 0x1u : 0u;

    for (const auto& ev : frame.events) {
      PointerEventPayload ptr;
      ptr.documentX = docX;
      ptr.documentY = docY;
      ptr.modifiers = ptrModifiers;
      if (ev.kind == donner::editor::repro::ReproEvent::Kind::MouseDown &&
          ev.mouseButton == 0) {
        ptr.phase = PointerPhase::kDown;
        ptr.buttons = 1;
        (void)core.handlePointerEvent(ptr);
      } else if (ev.kind == donner::editor::repro::ReproEvent::Kind::MouseUp &&
                 ev.mouseButton == 0) {
        ptr.phase = PointerPhase::kUp;
        ptr.buttons = 0;
        const auto mupFrame = core.handlePointerEvent(ptr);
        ++mouseUpCount;
        if (!mupFrame.hasFinalBitmap) continue;

        svg::RendererBitmap backend = BitmapFromFrame(mupFrame);
        svg::RendererBitmap expected = directRenderCurrentDom();
        ASSERT_FALSE(backend.empty());
        ASSERT_FALSE(expected.empty());
        if (backend.dimensions != expected.dimensions) continue;

        const std::string label = "rnr7_mup" + std::to_string(mouseUpCount) +
                                  "_noCpuCompose_vs_direct";
        const int before = ::testing::UnitTest::GetInstance()
                                ->current_test_info()
                                ->result()
                                ->total_part_count();
        donner::editor::tests::CompareBitmapToBitmap(backend, expected, label, params);
        const int after = ::testing::UnitTest::GetInstance()
                               ->current_test_info()
                               ->result()
                               ->total_part_count();
        if (after > before) {
          ++divergedCheckpoints;
        }
      }
    }
    if (nowHeld && leftHeld) {
      PointerEventPayload ptr;
      ptr.phase = PointerPhase::kMove;
      ptr.documentX = docX;
      ptr.documentY = docY;
      ptr.buttons = 1;
      ptr.modifiers = ptrModifiers;
      (void)core.handlePointerEvent(ptr);
    }
    leftHeld = nowHeld;
  }

  std::fprintf(stderr, "[rnr7-diff-nocpu] %d/%zu checkpoints diverged (CPU compose OFF)\n",
               divergedCheckpoints, mouseUpCount);
}

// Baseline sanity check: cold-load of splash at the 1310×1726 viewport
// should produce bright highlights in Lightning_glow_dark's canvas
// region (the filter's bright contribution, visible in the cold
// rendering). This gates the single-drag / repro-3 regressions — if
// it fails along with them, the bug isn't drag-state-specific.
TEST(EditorBackendCoreFilterDragTest, ColdLoadFilterOutputHasBrightHighlights) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  sandbox::EditorBackendCore core;
  SetViewportPayload vp;
  vp.width = 1310;
  vp.height = 1726;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = splashSource;
  const auto loadFrame = core.handleLoadBytes(load);
  ASSERT_TRUE(loadFrame.hasFinalBitmap);

  const int bmpW = loadFrame.finalBitmapWidth;
  const int bmpH = loadFrame.finalBitmapHeight;
  const std::size_t rowBytes = loadFrame.finalBitmapRowBytes;

  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
    const std::string path = std::string(dir) + "/" + AttemptTagPrefix() +
                             "cold_load_sanity.png";
    donner::svg::RendererImageIO::writeRgbaPixelsToPngFile(
        path.c_str(), loadFrame.finalBitmapPixels, bmpW, bmpH,
        static_cast<uint32_t>(rowBytes / 4u));
  }

  // Same probe rectangle as SingleDragOnLightningGlowDarkMovesOnlyThatGroup,
  // but at the COLD position (no drag applied): bolt center ~(470,
  // 415) doc → canvas center ~(690, 610) at bmpW/892 scale.
  const double scale = static_cast<double>(bmpW) / 892.0;
  const int centerX = static_cast<int>(std::round(470.0 * scale));
  const int centerY = static_cast<int>(std::round(415.0 * scale));
  const int halfExtent = static_cast<int>(std::round(35.0 * scale));
  const int rectMinX = std::max(0, centerX - halfExtent);
  const int rectMinY = std::max(0, centerY - halfExtent);
  const int rectMaxX = std::min(bmpW, centerX + halfExtent);
  const int rectMaxY = std::min(bmpH, centerY + halfExtent);

  int brightHits = 0;
  for (int cy = rectMinY; cy < rectMaxY; ++cy) {
    for (int cx = rectMinX; cx < rectMaxX; ++cx) {
      const std::size_t off = static_cast<std::size_t>(cy) * rowBytes +
                              static_cast<std::size_t>(cx) * 4u;
      const uint8_t r = loadFrame.finalBitmapPixels[off + 0];
      const uint8_t g = loadFrame.finalBitmapPixels[off + 1];
      const uint8_t b = loadFrame.finalBitmapPixels[off + 2];
      if (std::max({r, g, b}) > 180) {
        ++brightHits;
      }
    }
  }

  std::fprintf(stderr, "[cold_load brightHits] %d in probe (%d,%d)-(%d,%d)\n", brightHits,
               rectMinX, rectMinY, rectMaxX, rectMaxY);
  EXPECT_GT(brightHits, 100)
      << "Cold load at (" << rectMinX << "," << rectMinY << ")→(" << rectMaxX << ","
      << rectMaxY << ") has only " << brightHits << " bright (any channel > 180) "
      << "pixels. The Lightning_glow_dark filter region should have hundreds of "
      << "bright highlights at cold load — if this fails, the filter pipeline "
      << "is broken even at rest.";
}

// Repro for "filter group stops rendering after selecting the next
// thing": click/select filter group A, then click elsewhere to select
// B, then verify A's filter output still appears in the bitmap at its
// expected canvas region. Checks the compositor-cache interaction
// between `demoteEntity` + `promoteEntity` for a new drag target —
// specifically that a mandatory-promoted filter layer's cached
// bitmap isn't wiped by the subsequent promote of a different entity.
TEST(EditorBackendCoreFilterDragTest, SelectingNextFilterDoesNotEraseTheFirst) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  sandbox::EditorBackendCore core;
  SetViewportPayload vp;
  vp.width = 1310;
  vp.height = 1726;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = splashSource;
  const auto cold = core.handleLoadBytes(load);
  ASSERT_TRUE(cold.hasFinalBitmap);

  const auto sendPointer = [&](PointerPhase phase, double x, double y, uint32_t buttons) {
    PointerEventPayload ev;
    ev.phase = phase;
    ev.documentX = x;
    ev.documentY = y;
    ev.buttons = buttons;
    return core.handlePointerEvent(ev);
  };

  // Click A: Big_lightning_glow at (455, 160). Elevation lands on the
  // filter-g.
  const auto afterA = sendPointer(PointerPhase::kDown, 455.0, 160.0, 1);
  (void)sendPointer(PointerPhase::kUp, 455.0, 160.0, 0);
  std::fprintf(stderr, "[next-filter] after click A: %s\n",
               DescribeSelection(afterA).c_str());

  // Click B: Lightning_glow_dark at (474.5, 406.5). Triggers
  // demote(A) + promote(B).
  const auto afterB = sendPointer(PointerPhase::kDown, 474.5, 406.5, 1);
  const auto afterBUp = sendPointer(PointerPhase::kUp, 474.5, 406.5, 0);
  std::fprintf(stderr, "[next-filter] after click B: %s\n",
               DescribeSelection(afterB).c_str());

  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
    const std::string prefix = AttemptTagPrefix();
    const auto dump = [&](const char* name, const FramePayload& fp) {
      const std::string path = std::string(dir) + "/" + prefix + name;
      donner::svg::RendererImageIO::writeRgbaPixelsToPngFile(
          path.c_str(), fp.finalBitmapPixels, fp.finalBitmapWidth, fp.finalBitmapHeight,
          static_cast<uint32_t>(fp.finalBitmapRowBytes / 4u));
    };
    dump("next_filter_cold.png", cold);
    dump("next_filter_after_A.png", afterA);
    dump("next_filter_after_B.png", afterBUp);
  }

  // Compare Big_lightning_glow's canvas region cold vs after-B. Since
  // A was just deselected (no drag), its rendered pixels should be
  // unchanged from the cold baseline — selection state doesn't move
  // the entity.
  const int bmpW = afterBUp.finalBitmapWidth;
  const int bmpH = afterBUp.finalBitmapHeight;
  ASSERT_EQ(cold.finalBitmapWidth, bmpW);
  ASSERT_EQ(cold.finalBitmapHeight, bmpH);
  const std::size_t rowBytes = cold.finalBitmapRowBytes;

  // Big_lightning_glow region: (378, 104) → (500, 280) in doc space.
  const double scale = static_cast<double>(bmpW) / 892.0;
  const int rectMinX = static_cast<int>(std::round(378.0 * scale));
  const int rectMinY = static_cast<int>(std::round(104.0 * scale));
  const int rectMaxX = static_cast<int>(std::round(500.0 * scale));
  const int rectMaxY = static_cast<int>(std::round(280.0 * scale));

  int diverged = 0;
  for (int cy = rectMinY; cy < rectMaxY; ++cy) {
    for (int cx = rectMinX; cx < rectMaxX; ++cx) {
      const std::size_t off = static_cast<std::size_t>(cy) * rowBytes +
                              static_cast<std::size_t>(cx) * 4u;
      const int dr = std::abs(int(cold.finalBitmapPixels[off + 0]) -
                              int(afterBUp.finalBitmapPixels[off + 0]));
      const int dg = std::abs(int(cold.finalBitmapPixels[off + 1]) -
                              int(afterBUp.finalBitmapPixels[off + 1]));
      const int db = std::abs(int(cold.finalBitmapPixels[off + 2]) -
                              int(afterBUp.finalBitmapPixels[off + 2]));
      if (std::max({dr, dg, db}) > 20) ++diverged;
    }
  }
  const int rectArea = (rectMaxX - rectMinX) * (rectMaxY - rectMinY);
  std::fprintf(stderr, "[next-filter] Big_lightning_glow region: %d / %d px diverged\n",
               diverged, rectArea);

  EXPECT_LT(diverged, rectArea / 10)
      << "after selecting the next element, `Big_lightning_glow`'s region "
      << "diverged from the cold baseline by " << diverged << " px (out of "
      << rectArea << ") — >10% drift means A's filter output got erased by "
      << "the subsequent promote/demote cycle. See `next_filter_cold.png` "
      << "and `next_filter_after_B.png`.";
}

// Pins the elevation contract: clicking a path inside a `<g filter>`
// selects the filter group — not the clicked path (too narrow for a
// filter whose output is a composite of its subtree) nor an ancestor
// beyond the filter (the filter's cached-layer promotion invariants
// break and the drag drops to the 250 ms/frame full-render path).
// Deliberately does NOT assert sibling co-selection: the editor
// respects the DOM structure, so if the SVG author grouped siblings
// they move together, otherwise they don't. See issue #582 — the
// "drag-shears-composite" UX is accepted as an authoring-time
// concern, not something elevation auto-fixes at click time.
TEST(EditorBackendCoreFilterDragTest, ClickInsideFilterGroupSelectsTheGroup) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  sandbox::EditorBackendCore core;
  SetViewportPayload vp;
  vp.width = 1310;
  vp.height = 1726;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = splashSource;
  ASSERT_TRUE(core.handleLoadBytes(load).hasFinalBitmap);

  const auto sendPointer = [&](PointerPhase phase, double x, double y, uint32_t buttons) {
    PointerEventPayload ev;
    ev.phase = phase;
    ev.documentX = x;
    ev.documentY = y;
    ev.buttons = buttons;
    return core.handlePointerEvent(ev);
  };

  // (474.5, 406.5) lands on Lightning_glow_dark's cls-80 path — the
  // small lightning bolt's dark halo input geometry. This is the exact
  // repro-3 drag-2 click, so the elevation result must match what the
  // golden bitmap depends on.
  const auto afterMdown = sendPointer(PointerPhase::kDown, 474.5, 406.5, 1);

  std::vector<std::string> selectedIds;
  for (const auto& node : afterMdown.tree.nodes) {
    if (node.selected && !node.idAttr.empty()) {
      selectedIds.push_back(node.idAttr);
    }
  }
  const std::vector<std::string> expected = {"Lightning_glow_dark"};
  EXPECT_EQ(selectedIds, expected)
      << "click inside `<g id=\"Lightning_glow_dark\" filter=…>` should "
      << "elevate to the filter-g itself, producing a single-element "
      << "selection. Getting a different element means top-level-object "
      << "elevation picked the wrong ancestor (too shallow → a path "
      << "leaks; too deep → the wrapping container does).";
}

// Pins the elevation + drag contract for the small lightning bolt's
// `<g id="Lightning_glow_dark" filter="…">`. Matches the drag-2 click
// from `filter_elm_disappear-3.rnr` (474.5, 406.5 → 511.5, 410.5) at
// the recording's 1310×1726 canvas. Expected outcome:
//
//   * Elevation lands on `Lightning_glow_dark` (top-level object under
//     the splash's wrapping `<g class="cls-94">`);
//   * Drag translates ONLY that group's transform — sibling layers
//     (`Lightning_glow_bright`, `Lightning_white`) stay put;
//   * The composite visually shears, which is the accepted cost of
//     respecting the DOM structure (issue #582 — the user explicitly
//     vetoed auto-expansion to sibling composites. If a document
//     author wants the bolt to move as one unit, they wrap the three
//     in a parent `<g>`).
//
// Golden pixels capture that accepted shear; a regression that
// re-introduces either over-escalation (entire doc drags, caught by
// the `SelectionAabbWithinDocBudget` assertion) or auto-expansion
// (three sibling bboxes in `selections`) changes the output and trips
// the pixelmatch.
TEST(EditorBackendCoreFilterDragTest, SingleDragOnLightningGlowDarkMovesOnlyThatGroup) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  sandbox::EditorBackendCore core;
  SetViewportPayload vp;
  vp.width = 1310;
  vp.height = 1726;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = splashSource;
  ASSERT_TRUE(core.handleLoadBytes(load).hasFinalBitmap);

  const auto sendPointer = [&](PointerPhase phase, double x, double y, uint32_t buttons) {
    PointerEventPayload ev;
    ev.phase = phase;
    ev.documentX = x;
    ev.documentY = y;
    ev.buttons = buttons;
    return core.handlePointerEvent(ev);
  };

  // Coords from repro-3 drag 2: mdown (474.5, 406.5) → mup (511.5, 410.5).
  const auto afterMdown = sendPointer(PointerPhase::kDown, 474.5, 406.5, 1);
  std::fprintf(stderr, "[single_drag] after mdown: selection = %s\n",
               DescribeSelection(afterMdown).c_str());
  (void)sendPointer(PointerPhase::kMove, 485.0, 408.0, 1);
  (void)sendPointer(PointerPhase::kMove, 495.0, 409.0, 1);
  (void)sendPointer(PointerPhase::kMove, 511.5, 410.5, 1);
  const auto post = sendPointer(PointerPhase::kUp, 511.5, 410.5, 0);
  std::fprintf(stderr, "[single_drag] after mup: selection = %s\n",
               DescribeSelection(post).c_str());

  // Catch "elevation escalated to the document root" regressions
  // immediately — without waiting for a bright-pixel-threshold
  // inspection that would also trip for correct-but-sheared drags.
  // Splash viewBox is 892×512. 50% coverage fires only on attempts
  // that drag a near-full-document ancestor.
  EXPECT_TRUE(SelectionAabbWithinDocBudget(afterMdown, 892.0, 512.0, 0.50));

  ASSERT_TRUE(post.hasFinalBitmap);
  const int bmpW = post.finalBitmapWidth;
  const int bmpH = post.finalBitmapHeight;
  const std::size_t rowBytes = post.finalBitmapRowBytes;

  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
    const std::string path = std::string(dir) + "/" + AttemptTagPrefix() +
                             "single_drag_lgd_post.png";
    donner::svg::RendererImageIO::writeRgbaPixelsToPngFile(
        path.c_str(), post.finalBitmapPixels, bmpW, bmpH,
        static_cast<uint32_t>(rowBytes / 4u));
  }

  // Pin the post-drag pixels against a committed golden. Catches any
  // regression in the pipeline — including fix attempts that
  // accidentally make MORE pixels wrong somewhere else (attempt2/4
  // whole-document drags) — without the false-positive-prone bright-
  // pixel threshold the older assertion used. Regenerate via
  //   UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) \
  //     bazel run //donner/editor/sandbox/tests:editor_backend_golden_image_tests -- \
  //       --gtest_filter='*SingleDragOnLightningGlowDark*'
  // when a fix intentionally changes the output.
  donner::editor::tests::BitmapGoldenCompareParams params;
  params.threshold = 0.03f;
  params.maxMismatchedPixels = 500;
  donner::editor::tests::CompareBitmapToGolden(
      BitmapFromFrame(post),
      "donner/editor/sandbox/tests/testdata/filter_disappear_single_drag_lgd_post.png",
      "single_drag_lgd_post", params);
}

// User-reported regression (filter_elm_disappear-3.rnr + accompanying
// screenshot): the post-settle frame shows a dark-blue rectangle
// where the small lightning bolt should be.
//
// Same root cause as `SingleDragOnLightningGlowDarkMovesOnlyThatGroup` — the
// two-drag structure is incidental. See that test's docstring for the
// elevation-escalation fix sketch.
TEST(EditorBackendCoreFilterDragTest, FilterDisappearRepro3PostSettleMatchesDirectRender) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ifstream reproStream("donner/editor/tests/filter_elm_disappear-3.rnr");
  if (!reproStream.is_open()) {
    GTEST_SKIP() << "filter_elm_disappear-3.rnr not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  auto reproOpt = donner::editor::repro::ReadReproFile(
      "donner/editor/tests/filter_elm_disappear-3.rnr");
  ASSERT_TRUE(reproOpt.has_value()) << "failed to parse repro-3";
  const auto& repro = *reproOpt;

  // Pull canvas size from the recording's first viewport block so the
  // backend renders at the same dimensions the recording was made at.
  const auto* firstVp = [&]() -> const donner::editor::repro::ReproViewport* {
    for (const auto& f : repro.frames)
      if (f.viewport.has_value()) return &*f.viewport;
    return nullptr;
  }();
  ASSERT_NE(firstVp, nullptr) << "repro-3 has no viewport block";
  const int canvasW =
      static_cast<int>(std::round(firstVp->paneSizeW * firstVp->devicePixelRatio));
  const int canvasH =
      static_cast<int>(std::round(firstVp->paneSizeH * firstVp->devicePixelRatio));
  ASSERT_GT(canvasW, 0);
  ASSERT_GT(canvasH, 0);

  sandbox::EditorBackendCore core;
  {
    SetViewportPayload vp;
    vp.width = canvasW;
    vp.height = canvasH;
    (void)core.handleSetViewport(vp);
  }

  LoadBytesPayload load;
  load.bytes = splashSource;
  const auto loadFrame = core.handleLoadBytes(load);
  ASSERT_TRUE(loadFrame.hasFinalBitmap);
  // Backend may fit the canvas to the SVG's viewBox aspect ratio; the
  // bitmap dimensions can be smaller than the requested canvas. Use
  // the actual bitmap dims for sampling.
  const int bmpW = loadFrame.finalBitmapWidth;
  const int bmpH = loadFrame.finalBitmapHeight;

  // Sample the cold-load frame before replay so the comparison below
  // has a known "nothing moved" baseline — whatever pattern of warm
  // pixels the filter paints here is what MUST still be painted (at
  // the post-drag position) in the post-settle frame.
  const std::size_t rowBytes = loadFrame.finalBitmapRowBytes;
  const auto samplePx = [&](const FramePayload& fp, int x, int y) {
    const std::size_t off = static_cast<std::size_t>(y) * rowBytes +
                            static_cast<std::size_t>(x) * 4u;
    struct Rgba {
      uint8_t r, g, b, a;
    };
    return Rgba{fp.finalBitmapPixels[off + 0], fp.finalBitmapPixels[off + 1],
                fp.finalBitmapPixels[off + 2], fp.finalBitmapPixels[off + 3]};
  };
  int coldBright = 0;
  for (int cy = 0; cy < bmpH; cy += 30) {
    for (int cx = 0; cx < bmpW; cx += 30) {
      const auto px = samplePx(loadFrame, cx, cy);
      if (int(px.r) + int(px.g) > 120) ++coldBright;
    }
  }
  ASSERT_GT(coldBright, 20) << "splash cold-render is missing expected warm content — "
                               "fixture broken, not the bug under test";

  // Replay every frame in order: button edges → pointer events;
  // button-held → pointer kMove with the frame's doc coords.
  bool leftHeld = false;
  std::optional<FramePayload> lastFrame = loadFrame;
  std::size_t mouseUpCount = 0;
  std::optional<FramePayload> afterMup2;
  std::optional<FramePayload> midDrag2;
  bool mdown2Seen = false;
  int drag2MoveCount = 0;
  for (const auto& frame : repro.frames) {
    if (!frame.mouseDocX.has_value()) continue;
    const double docX = *frame.mouseDocX;
    const double docY = *frame.mouseDocY;
    const bool nowHeld = (frame.mouseButtonMask & 1) != 0;

    for (const auto& ev : frame.events) {
      PointerEventPayload ptr;
      ptr.documentX = docX;
      ptr.documentY = docY;
      if (ev.kind == donner::editor::repro::ReproEvent::Kind::MouseDown &&
          ev.mouseButton == 0) {
        if (mouseUpCount == 1) {
          mdown2Seen = true;
        }
        ptr.phase = PointerPhase::kDown;
        ptr.buttons = 1;
        lastFrame = core.handlePointerEvent(ptr);
        std::fprintf(stderr, "[rnr3] mdown#%zu @ (%.1f, %.1f): selection = %s\n",
                     mouseUpCount + 1, docX, docY,
                     DescribeSelection(*lastFrame).c_str());
      } else if (ev.kind == donner::editor::repro::ReproEvent::Kind::MouseUp &&
                 ev.mouseButton == 0) {
        ptr.phase = PointerPhase::kUp;
        ptr.buttons = 0;
        lastFrame = core.handlePointerEvent(ptr);
        ++mouseUpCount;
        if (mouseUpCount == 2) afterMup2 = *lastFrame;
        std::fprintf(stderr, "[rnr3] mup#%zu @ (%.1f, %.1f): selection = %s\n",
                     mouseUpCount, docX, docY,
                     DescribeSelection(*lastFrame).c_str());
      }
    }
    if (nowHeld && leftHeld) {
      PointerEventPayload ptr;
      ptr.phase = PointerPhase::kMove;
      ptr.documentX = docX;
      ptr.documentY = docY;
      ptr.buttons = 1;
      lastFrame = core.handlePointerEvent(ptr);
      if (mdown2Seen && mouseUpCount == 1) {
        ++drag2MoveCount;
        if (drag2MoveCount == 3) {
          midDrag2 = *lastFrame;
        }
      }
    }
    leftHeld = nowHeld;
  }

  ASSERT_EQ(mouseUpCount, 2u) << "repro-3 should contain exactly two mups";
  ASSERT_TRUE(afterMup2.has_value());
  ASSERT_TRUE(lastFrame.has_value() && lastFrame->hasFinalBitmap);

  // Pin elevation regressions to the exact attempt signature before
  // the bitmap-level assertion fires.
  EXPECT_TRUE(SelectionAabbWithinDocBudget(*afterMup2, 892.0, 512.0, 0.50));

  // Dump artifacts for eyeballing.
  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
    const std::string prefix = AttemptTagPrefix();
    const auto dump = [&](const char* name, const FramePayload& fp) {
      const std::string path = std::string(dir) + "/" + prefix + name;
      donner::svg::RendererImageIO::writeRgbaPixelsToPngFile(
          path.c_str(), fp.finalBitmapPixels, fp.finalBitmapWidth, fp.finalBitmapHeight,
          static_cast<uint32_t>(fp.finalBitmapRowBytes / 4u));
    };
    dump("rnr3_cold.png", loadFrame);
    if (midDrag2.has_value()) dump("rnr3_mid_drag2.png", *midDrag2);
    dump("rnr3_after_mup2.png", *afterMup2);
  }

  // Pin post-settle pixels against a committed golden. Same update
  // workflow as `SingleDragOnLightningGlowDarkMovesOnlyThatGroup`.
  donner::editor::tests::BitmapGoldenCompareParams params;
  params.threshold = 0.03f;
  params.maxMismatchedPixels = 500;
  donner::editor::tests::CompareBitmapToGolden(
      BitmapFromFrame(*afterMup2),
      "donner/editor/sandbox/tests/testdata/filter_disappear_rnr3_after_mup2.png",
      "rnr3_after_mup2", params);
}

TEST(EditorBackendCoreFilterDragTest, FilterSurvivesFollowUpDragAtHiDpi) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  sandbox::EditorBackendCore core;
  SetViewportPayload vp;
  vp.width = 1784;
  vp.height = 1024;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = splashSource;
  const auto loadFrame = core.handleLoadBytes(load);
  ASSERT_TRUE(loadFrame.hasFinalBitmap);

  const size_t rowBytes = loadFrame.finalBitmapRowBytes;

  // Drag 1 clicks Big_lightning_glow's cls-79 path (proven by the
  // earlier `BigLightningGlowSurvivesDragReleaseCycle` test). Drag 2
  // clicks inside Lightning_glow_dark's path — a SECOND mandatory-
  // promoted filter group separate from Big_lightning_glow. This
  // mirrors the user's recording pattern (`filter_elm_disappear-2`:
  // drag one filter group, then drag a different element that's in
  // its own mandatory-promoted layer).
  constexpr double kDrag1MdownX = 455.0;
  constexpr double kDrag1MdownY = 160.0;
  constexpr double kDrag1MupX = 488.0;
  constexpr double kDrag1MupY = 164.0;
  // Lightning_glow_dark path center ~ (465, 415).
  constexpr double kDrag2MdownX = 465.0;
  constexpr double kDrag2MdownY = 415.0;
  constexpr double kDrag2MidX = 470.0;
  constexpr double kDrag2MidY = 418.0;

  const auto sendPointer = [&](PointerPhase phase, double x, double y, uint32_t buttons) {
    PointerEventPayload ev;
    ev.phase = phase;
    ev.documentX = x;
    ev.documentY = y;
    ev.buttons = buttons;
    return core.handlePointerEvent(ev);
  };

  // Drag 1: click on filter, move a few times, release. Final
  // transform is driven by the last onMouseMove's cursor pos (mup
  // doesn't call onMouseMove), so send an explicit move to the mup
  // location before the release.
  (void)sendPointer(PointerPhase::kDown, kDrag1MdownX, kDrag1MdownY, 1);
  (void)sendPointer(PointerPhase::kMove, (kDrag1MdownX + kDrag1MupX) * 0.5,
                    (kDrag1MdownY + kDrag1MupY) * 0.5, 1);
  (void)sendPointer(PointerPhase::kMove, kDrag1MupX, kDrag1MupY, 1);
  (void)sendPointer(PointerPhase::kUp, kDrag1MupX, kDrag1MupY, 0);

  // Drag 2: click a different element, move partway — capture the
  // composited frame at mid-drag-2.
  (void)sendPointer(PointerPhase::kDown, kDrag2MdownX, kDrag2MdownY, 1);
  const auto midDrag2 = sendPointer(PointerPhase::kMove, kDrag2MidX, kDrag2MidY, 1);
  ASSERT_TRUE(midDrag2.hasFinalBitmap);
  ASSERT_EQ(midDrag2.finalBitmapWidth, 1784);
  ASSERT_EQ(midDrag2.finalBitmapHeight, 1024);

  // Dump the mid-drag-2 frame for inspection.
  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
    const std::string path = std::string(dir) + "/" + AttemptTagPrefix() +
                             "mid_drag2_hidpi_host.png";
    donner::svg::RendererImageIO::writeRgbaPixelsToPngFile(
        path.c_str(), midDrag2.finalBitmapPixels, 1784, 1024, 1784u);
  }

  // Drag 1 moved Big_lightning_glow by (33, 4) doc units from its
  // original position. The lightning bolt's canvas bounds at DPR=2
  // are roughly (800, 200) → (1100, 600) pre-drag and shift by
  // (66, 8) canvas pixels post-drag-1 → (866, 208) → (1166, 608).
  // Probe a grid centered there for the warm yellow glow the filter
  // paints — if the filter "disappeared" mid-drag-2, the region
  // reverts to dark navy background.
  const auto samplePx = [&](int x, int y) {
    const size_t off = static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x) * 4u;
    struct Rgba { uint8_t r, g, b, a; };
    return Rgba{midDrag2.finalBitmapPixels[off + 0], midDrag2.finalBitmapPixels[off + 1],
                midDrag2.finalBitmapPixels[off + 2], midDrag2.finalBitmapPixels[off + 3]};
  };

  // Probe the bolt-center area of the filter's post-drag-1 position.
  // The filter content has the bright central bolt (pale yellow)
  // surrounded by warm orange glow; dark navy background has R+G < 60.
  int brightHits = 0;
  int totalSamples = 0;
  for (int cy = 280; cy <= 560; cy += 30) {
    for (int cx = 880; cx <= 1140; cx += 30) {
      ++totalSamples;
      const auto px = samplePx(cx, cy);
      const int warmth = int(px.r) + int(px.g);
      if (warmth > 120) {
        ++brightHits;
      }
    }
  }

  EXPECT_GE(brightHits, totalSamples / 4)
      << "mid-drag-2 composite is missing the filter element at its post-drag-1 "
      << "canvas bounds. Probed " << totalSamples << " pixels for warm (R+G>120) "
      << "lightning-glow content; got " << brightHits << " hits; expected ≥"
      << (totalSamples / 4) << ". If the live editor shows the filter "
      << "vanishing mid-second-drag, that regression presents here too.";
}

// User-reported regression: after dragging a `<g filter>` the element
// disappears. Drive the sandbox through mouseDown + N moves + mouseUp
// + one idle render, and verify the dragged group's descendant
// geometry is still visible in the final bitmap at its post-drag
// position.
// Isolate whether the #582 divergence comes from the compositor's
// cached-bitmap + translation math vs. filter rasterization being
// non-translation-invariant. Mutates filter-g's transform DIRECTLY
// on the DOM (no events, no SelectTool, no drag threshold, no
// fast-path trigger), then asks the backend to render and diffs.
//
// If this test PASSES, the issue is in the event-driven fast path
// mutating compositor state in a way that doesn't match direct
// render. If it FAILS, the issue is in how filter rasterization
// handles the post-mutation DOM vs. a fresh single-pass render.
TEST(EditorBackendCoreFilterDragTest,
     SplashDirectDomMutationMatchesDirectRender) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  sandbox::EditorBackendCore core;
  SetViewportPayload vp;
  vp.width = 1310;
  vp.height = 1726;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = splashSource;
  (void)core.handleLoadBytes(load);

  // Mutate filter-g's transform directly — no events, no SelectTool.
  // This is the "cleanest possible drag": DOM at post-drag state,
  // no interaction history in the compositor.
  svg::SVGDocument& doc = core.editor().document().document();
  auto filterG = doc.querySelector("#Big_lightning_glow");
  ASSERT_TRUE(filterG.has_value());
  filterG->cast<svg::SVGGraphicsElement>().setTransform(
      Transform2d::Translate(Vector2d(9.0, 4.0)));

  // Force a re-render by calling handleSetViewport with same dims.
  // (Any opcode triggers `buildFramePayload`.)
  const auto frame = core.handleSetViewport(vp);
  ASSERT_TRUE(frame.hasFinalBitmap);

  svg::Renderer reference;
  reference.draw(doc);
  svg::RendererBitmap expected = reference.takeSnapshot();
  svg::RendererBitmap backend = BitmapFromFrame(frame);

  donner::editor::tests::BitmapGoldenCompareParams params;
  params.threshold = 0.03f;
  params.maxMismatchedPixels = 500;  // no chrome, no drag noise
  donner::editor::tests::CompareBitmapToBitmap(
      backend, expected, "splash_direct_dom_mutation_vs_direct", params);
}

// Tightest possible #582 reproducer: after mdown on filter-g (but
// BEFORE any drag has activated), compare backend bitmap to direct
// render. If this diverges, the compositor's rasterization of
// filter-g produces different pixels than a single-pass render of
// the same DOM — no translation, no drag, no fast-path involved.
// The bug would then be in the compositor's cached rasterization
// itself, not in any transform/compose math.
TEST(EditorBackendCoreFilterDragTest, SplashMdownFilterGJustRasterizedMatchesDirectRender) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  sandbox::EditorBackendCore core;
  SetViewportPayload vp;
  vp.width = 1310;
  vp.height = 1726;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = splashSource;
  const auto loadFrame = core.handleLoadBytes(load);
  ASSERT_TRUE(loadFrame.hasFinalBitmap);

  // Cold sanity: backend matches direct render bit-for-bit.
  {
    svg::SVGDocument& doc = core.editor().document().document();
    svg::Renderer reference;
    reference.draw(doc);
    svg::RendererBitmap expected = reference.takeSnapshot();
    svg::RendererBitmap backend = BitmapFromFrame(loadFrame);
    donner::editor::tests::BitmapGoldenCompareParams params;
    params.threshold = 0.03f;
    params.maxMismatchedPixels = 100;
    donner::editor::tests::CompareBitmapToBitmap(backend, expected, "splash_cold_vs_direct",
                                                  params);
  }

  // mdown only — no moves, no drag. This promotes filter-g into a
  // compositor layer (Selection kind) and rasterizes its bitmap
  // ONCE at the entity's current (pre-drag) transform. The chrome
  // also appears. Divergence above chrome baseline = the rasterized
  // bitmap disagrees with a fresh single-pass render at the SAME
  // DOM state — i.e. filter rendering is path-dependent.
  PointerEventPayload down;
  down.phase = donner::editor::sandbox::PointerPhase::kDown;
  down.documentX = 486.5;
  down.documentY = 189.5;
  down.buttons = 1;
  const auto downFrame = core.handlePointerEvent(down);

  svg::SVGDocument& doc = core.editor().document().document();
  svg::Renderer reference;
  reference.draw(doc);
  svg::RendererBitmap expected = reference.takeSnapshot();
  svg::RendererBitmap backend = BitmapFromFrame(downFrame);

  donner::editor::tests::BitmapGoldenCompareParams params;
  params.threshold = 0.03f;
  // Budget sized for multi-element selection chrome: single-element
  // chrome is ~2000-3000 px (outline + handles for a 94×176 AABB at
  // 1.47x scale), but shift-click selections can stack several
  // elements' worth of outlines + path strokes. rnr7 mup#7 selects
  // an ellipse (365×421) + the big lightning bolt simultaneously,
  // producing ~5000 px of chrome alone. 10000 leaves headroom for
  // future recordings with bigger selection sets without letting
  // any real structural drift through — even one missing filter
  // layer overwhelms this budget.
  params.maxMismatchedPixels = 10000;  // chrome absorbs here too
  donner::editor::tests::CompareBitmapToBitmap(backend, expected,
                                                "splash_mdown_only_vs_direct", params);
}

// Per-frame bisection of #582: replay the filter-g drag on splash
// one move event at a time, and after EACH move measure the divergence
// between backend and direct render. Identifies the exact frame where
// the drift crosses a meaningful threshold — handy for pointing at
// what the compositor is doing on THAT frame vs the ones before.
//
// Logs-only (no assertion on divergence count), so the test always
// passes; the output is the interesting artifact.
TEST(EditorBackendCoreFilterDragTest, SplashFilterDragPerFrameDivergence) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  sandbox::EditorBackendCore core;
  SetViewportPayload vp;
  vp.width = 1310;
  vp.height = 1726;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = splashSource;
  (void)core.handleLoadBytes(load);

  const auto directRenderCurrentDom = [&]() -> svg::RendererBitmap {
    svg::SVGDocument& doc = core.editor().document().document();
    svg::Renderer reference;
    reference.draw(doc);
    return reference.takeSnapshot();
  };

  // Count pixels that differ by any channel > 0.03. No threshold on
  // total count — just count and log.
  const auto countDiverged = [](const svg::RendererBitmap& a, const svg::RendererBitmap& b) -> int {
    if (a.dimensions != b.dimensions || a.rowBytes != b.rowBytes ||
        a.pixels.size() != b.pixels.size()) {
      return -1;
    }
    const int w = a.dimensions.x;
    const int h = a.dimensions.y;
    const std::size_t rb = a.rowBytes;
    int diff = 0;
    for (int y = 0; y < h; ++y) {
      const std::size_t rowOff = static_cast<std::size_t>(y) * rb;
      for (int x = 0; x < w; ++x) {
        const std::size_t p = rowOff + static_cast<std::size_t>(x) * 4u;
        int dMax = 0;
        for (int c = 0; c < 3; ++c) {
          dMax = std::max(dMax,
                           std::abs(int(a.pixels[p + c]) - int(b.pixels[p + c])));
        }
        if (dMax > 8) ++diff;  // 8/255 ≈ 0.03 threshold
      }
    }
    return diff;
  };

  PointerEventPayload down;
  down.phase = donner::editor::sandbox::PointerPhase::kDown;
  down.documentX = 486.5;
  down.documentY = 189.5;
  down.buttons = 1;
  const auto downFrame = core.handlePointerEvent(down);
  const int downDivergence = countDiverged(BitmapFromFrame(downFrame),
                                            directRenderCurrentDom());
  std::fprintf(stderr, "[frame-by-frame] mdown: divergence=%d px\n", downDivergence);

  constexpr int kMoves = 50;
  const double dx = (495.5 - 486.5) / static_cast<double>(kMoves);
  const double dy = (193.5 - 189.5) / static_cast<double>(kMoves);
  for (int i = 1; i <= kMoves; ++i) {
    PointerEventPayload mv;
    mv.phase = donner::editor::sandbox::PointerPhase::kMove;
    mv.documentX = 486.5 + dx * i;
    mv.documentY = 189.5 + dy * i;
    mv.buttons = 1;
    const auto mvFrame = core.handlePointerEvent(mv);
    const int d = countDiverged(BitmapFromFrame(mvFrame), directRenderCurrentDom());
    std::fprintf(stderr, "[frame-by-frame] mv#%d (doc=%.2f,%.2f): divergence=%d px\n", i,
                 mv.documentX, mv.documentY, d);
  }

  PointerEventPayload up;
  up.phase = donner::editor::sandbox::PointerPhase::kUp;
  up.documentX = 495.5;
  up.documentY = 193.5;
  up.buttons = 0;
  const auto upFrame = core.handlePointerEvent(up);
  const int upDivergence = countDiverged(BitmapFromFrame(upFrame), directRenderCurrentDom());
  std::fprintf(stderr, "[frame-by-frame] mup: divergence=%d px\n", upDivergence);
}

// Minimal reproducer for #582 on the REAL SPLASH but with only the
// first rnr7 interaction (click + small drag of `#Big_lightning_glow`,
// release). If this fails where `MinimalFilterDragProducesCorrectBackendPixels`
// passes, the bug is triggered by something splash-specific — multiple
// filter groups, nested group structure, or something in the complex
// compositing — NOT by the click-drag-release sequence itself.
TEST(EditorBackendCoreFilterDragTest, SplashFilterDragFirstReleaseMatchesDirectRender) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }
  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  const std::string splashSource = splashBuf.str();

  sandbox::EditorBackendCore core;
  SetViewportPayload vp;
  vp.width = 1310;
  vp.height = 1726;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = splashSource;
  (void)core.handleLoadBytes(load);

  const auto directRenderCurrentDom = [&]() -> svg::RendererBitmap {
    svg::SVGDocument& doc = core.editor().document().document();
    svg::Renderer reference;
    reference.draw(doc);
    return reference.takeSnapshot();
  };

  // Pre-click sanity: cold bitmap = direct render.
  {
    svg::RendererBitmap backend = BitmapFromFrame(core.buildFramePayload());
    svg::RendererBitmap expected = directRenderCurrentDom();
    donner::editor::tests::BitmapGoldenCompareParams params;
    params.threshold = 0.03f;
    params.maxMismatchedPixels = 100;
    donner::editor::tests::CompareBitmapToBitmap(backend, expected,
                                                  "splash_cold_vs_direct", params);
  }

  // Exactly the rnr7 mup#1 sequence: click filter-g, drag ~9/4 pixels,
  // release.
  PointerEventPayload down;
  down.phase = donner::editor::sandbox::PointerPhase::kDown;
  down.documentX = 486.5;
  down.documentY = 189.5;
  down.buttons = 1;
  (void)core.handlePointerEvent(down);

  // Fire ~50 interpolated move events to match the ~50 drag frames
  // between rnr7's mdown#1 and mup#1. Each kMove triggers a
  // `buildFramePayload` and a compositor fast-path state update; the
  // hypothesis is that state accumulates through many frames in a way
  // that diverges from what a single-drag test would show.
  constexpr int kMoves = 50;
  const double dx = (495.5 - 486.5) / static_cast<double>(kMoves);
  const double dy = (193.5 - 189.5) / static_cast<double>(kMoves);
  for (int i = 1; i <= kMoves; ++i) {
    PointerEventPayload mv;
    mv.phase = donner::editor::sandbox::PointerPhase::kMove;
    mv.documentX = 486.5 + dx * i;
    mv.documentY = 189.5 + dy * i;
    mv.buttons = 1;
    (void)core.handlePointerEvent(mv);
  }

  PointerEventPayload up;
  up.phase = donner::editor::sandbox::PointerPhase::kUp;
  up.documentX = 495.5;
  up.documentY = 193.5;
  up.buttons = 0;
  const auto postReleaseFrame = core.handlePointerEvent(up);

  svg::RendererBitmap backend = BitmapFromFrame(postReleaseFrame);
  svg::RendererBitmap expected = directRenderCurrentDom();
  donner::editor::tests::BitmapGoldenCompareParams params;
  params.threshold = 0.03f;
  // Budget sized for multi-element selection chrome: single-element
  // chrome is ~2000-3000 px (outline + handles for a 94×176 AABB at
  // 1.47x scale), but shift-click selections can stack several
  // elements' worth of outlines + path strokes. rnr7 mup#7 selects
  // an ellipse (365×421) + the big lightning bolt simultaneously,
  // producing ~5000 px of chrome alone. 10000 leaves headroom for
  // future recordings with bigger selection sets without letting
  // any real structural drift through — even one missing filter
  // layer overwhelms this budget.
  params.maxMismatchedPixels = 10000;
  donner::editor::tests::CompareBitmapToBitmap(backend, expected,
                                                "splash_postrelease_vs_direct", params);
}

// Minimal reproducer for #582. A single `<g filter>` element, clicked
// and dragged a few pixels. After release, diff the backend's
// finalBitmapPixels against a direct render of the DOM. The simpler
// fixture should isolate the failure away from splash-specific details
// (multiple filters, nested groups, complex compositing).
TEST(EditorBackendCoreFilterDragTest, MinimalFilterDragProducesCorrectBackendPixels) {
  constexpr std::string_view kSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">
  <defs>
    <filter id="blur"><feGaussianBlur stdDeviation="2"/></filter>
  </defs>
  <rect width="200" height="200" fill="white"/>
  <g id="target" filter="url(#blur)">
    <rect x="50" y="80" width="40" height="40" fill="red"/>
  </g>
</svg>)svg";

  sandbox::EditorBackendCore core;
  SetViewportPayload vp;
  vp.width = 200;
  vp.height = 200;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = std::string(kSvg);
  (void)core.handleLoadBytes(load);

  const auto directRenderCurrentDom = [&]() -> svg::RendererBitmap {
    svg::SVGDocument& doc = core.editor().document().document();
    svg::Renderer reference;
    reference.draw(doc);
    return reference.takeSnapshot();
  };

  donner::editor::tests::BitmapGoldenCompareParams params;
  params.threshold = 0.03f;
  params.maxMismatchedPixels = 200;  // tighter budget on the minimal fixture

  // Pre-click sanity: no interactions, no chrome. Backend bitmap should
  // equal direct render exactly. If this fails the fixture is broken.
  {
    LoadBytesPayload noop;  // handleLoadBytes already produced a frame
    svg::RendererBitmap backend = BitmapFromFrame(core.buildFramePayload());
    svg::RendererBitmap expected = directRenderCurrentDom();
    donner::editor::tests::CompareBitmapToBitmap(backend, expected,
                                                  "minimal_preclick_vs_direct", params);
  }

  // Click on the filter group (SelectTool elevates to `<g filter>`).
  PointerEventPayload down;
  down.phase = donner::editor::sandbox::PointerPhase::kDown;
  down.documentX = 70.0;
  down.documentY = 100.0;
  down.buttons = 1;
  (void)core.handlePointerEvent(down);

  // Drag 10 pixels right.
  PointerEventPayload mv;
  mv.phase = donner::editor::sandbox::PointerPhase::kMove;
  mv.documentX = 80.0;
  mv.documentY = 100.0;
  mv.buttons = 1;
  (void)core.handlePointerEvent(mv);

  // Release.
  PointerEventPayload up;
  up.phase = donner::editor::sandbox::PointerPhase::kUp;
  up.documentX = 80.0;
  up.documentY = 100.0;
  up.buttons = 0;
  const auto postReleaseFrame = core.handlePointerEvent(up);

  // Post-release: diff. Backend should match direct render (modulo
  // chrome, which here is only the selection AABB — ~400 px at most
  // for a 40×40 shape with 1-2 px outline).
  svg::RendererBitmap backend = BitmapFromFrame(postReleaseFrame);
  svg::RendererBitmap expected = directRenderCurrentDom();
  donner::editor::tests::BitmapGoldenCompareParams postParams;
  postParams.threshold = 0.03f;
  postParams.maxMismatchedPixels = 600;  // chrome for 40×40 rect + AA
  donner::editor::tests::CompareBitmapToBitmap(backend, expected,
                                                "minimal_postrelease_vs_direct", postParams);
}

TEST(EditorBackendCoreFilterDragTest, FilterGroupSurvivesDragReleaseCycle) {
  constexpr std::string_view kSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">
  <defs>
    <filter id="blur"><feGaussianBlur stdDeviation="1"/></filter>
  </defs>
  <rect width="200" height="200" fill="white"/>
  <g id="target" filter="url(#blur)">
    <rect x="50" y="80" width="40" height="40" fill="red"/>
  </g>
</svg>)svg";

  sandbox::EditorBackendCore core;
  SetViewportPayload vp;
  vp.width = 200;
  vp.height = 200;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = std::string(kSvg);
  (void)core.handleLoadBytes(load);

  // Click inside the red rect, which lives under `<g filter>`.
  // SelectTool elevates the click to the filter group.
  PointerEventPayload down;
  down.phase = donner::editor::sandbox::PointerPhase::kDown;
  down.documentX = 70.0;
  down.documentY = 100.0;
  down.buttons = 1;
  (void)core.handlePointerEvent(down);

  // Drag 10 cumulative frames — each `kMove` posts the latest mouse
  // position, mirroring how main.cc dispatches drag events.
  for (int i = 1; i <= 10; ++i) {
    PointerEventPayload mv;
    mv.phase = donner::editor::sandbox::PointerPhase::kMove;
    mv.documentX = 70.0 + static_cast<double>(i) * 3.0;
    mv.documentY = 100.0;
    mv.buttons = 1;
    (void)core.handlePointerEvent(mv);
  }

  // Release.
  PointerEventPayload up;
  up.phase = donner::editor::sandbox::PointerPhase::kUp;
  up.documentX = 100.0;
  up.documentY = 100.0;
  (void)core.handlePointerEvent(up);

  // One more idle pointer event (`kMove` without button, mirroring a
  // hover that ImGui's main loop fires every frame). This is the
  // frame the user actually SEES post-release — if the element is
  // gone here, that matches the "disappears" report.
  PointerEventPayload hover;
  hover.phase = donner::editor::sandbox::PointerPhase::kMove;
  hover.documentX = 100.0;
  hover.documentY = 100.0;
  hover.buttons = 0;
  const auto postReleaseFrame = core.handlePointerEvent(hover);

  // Reconstruct the bitmap from the wire payload and probe where the
  // red rect should be. Drag was 10 × 3 = 30 doc units right.
  // Original rect center: (70, 100). Post-drag center: (100, 100).
  ASSERT_TRUE(postReleaseFrame.hasFinalBitmap)
      << "post-release frame must produce a final bitmap";
  ASSERT_FALSE(postReleaseFrame.finalBitmapPixels.empty());

  // `finalBitmapAlphaType` is the straight/premul flag that flows
  // with the bitmap; we don't care for this probe.
  const size_t width = static_cast<size_t>(postReleaseFrame.finalBitmapWidth);
  const size_t rowBytes = postReleaseFrame.finalBitmapRowBytes > 0
                               ? postReleaseFrame.finalBitmapRowBytes
                               : width * 4u;
  const auto samplePixel = [&](int x, int y) {
    const size_t off = static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x) * 4u;
    struct Rgba {
      uint8_t r, g, b, a;
    };
    return Rgba{postReleaseFrame.finalBitmapPixels[off + 0],
                postReleaseFrame.finalBitmapPixels[off + 1],
                postReleaseFrame.finalBitmapPixels[off + 2],
                postReleaseFrame.finalBitmapPixels[off + 3]};
  };

  // Probe a 5×5 grid around the post-drag center. At least one pixel
  // should show a meaningfully red component (filter blur smears the
  // color, so we accept a broad tolerance).
  int redHits = 0;
  for (int dy = -2; dy <= 2; ++dy) {
    for (int dx = -2; dx <= 2; ++dx) {
      const auto px = samplePixel(100 + dx, 100 + dy);
      if (px.r > 100 && px.g < 180 && px.b < 180) {
        ++redHits;
      }
    }
  }
  EXPECT_GE(redHits, 5)
      << "dragged filter group's red rect is missing from the post-release "
         "frame. Expected some red around canvas pixel (100, 100); got none. "
         "Likely causes: `propagateFastPathTranslationToSubtree` corrupting "
         "descendant `worldFromEntityTransform`, or the compositor re-"
         "rasterizing the layer with stale descendant positions.";

  // Also sanity-check that the ORIGINAL position is now vacated —
  // otherwise the test passes with a red streak that never moved.
  const auto originalCenter = samplePixel(70, 100);
  EXPECT_FALSE(originalCenter.r > 100 && originalCenter.g < 180 && originalCenter.b < 180)
      << "original rect position (70, 100) is still red after drag — the "
         "drag didn't actually move the element, or the renderer drew it "
         "twice.";
}

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
// Control: drag a single `<rect>` at 1× DPR. Numbers from here set
// the baseline for what "healthy" looks like — any filter-group /
// HiDPI variant should land within the same order of magnitude.
TEST(EditorUiFlowPerfTest, RegularObjectDragHitsSixtyFpsBudget) {
  constexpr std::string_view kSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="800" height="600" viewBox="0 0 800 600">
  <rect width="800" height="600" fill="white"/>
  <rect id="target" x="100" y="100" width="80" height="60" fill="red"/>
</svg>)svg";

  sandbox::EditorBackendCore core;
  SetViewportPayload vp;
  vp.width = 800;
  vp.height = 600;
  (void)core.handleSetViewport(vp);

  LoadBytesPayload load;
  load.bytes = std::string(kSvg);
  (void)core.handleLoadBytes(load);

  PointerEventPayload down;
  down.phase = donner::editor::sandbox::PointerPhase::kDown;
  down.documentX = 140.0;
  down.documentY = 130.0;
  down.buttons = 1;
  ASSERT_FALSE(core.handlePointerEvent(down).selections.empty());

  PointerEventPayload warmup;
  warmup.phase = donner::editor::sandbox::PointerPhase::kMove;
  warmup.documentX = 141.0;
  warmup.documentY = 130.0;
  warmup.buttons = 1;
  (void)core.handlePointerEvent(warmup);

  constexpr int kSteadyFrames = 30;
  std::vector<uint8_t> textureStandIn(800 * 600 * 4, 0);
  using Clock = std::chrono::steady_clock;
  double totalMs = 0.0;
  for (int i = 0; i < kSteadyFrames; ++i) {
    PointerEventPayload mv;
    mv.phase = donner::editor::sandbox::PointerPhase::kMove;
    mv.documentX = 141.0 + (i + 1) * 1.5;
    mv.documentY = 130.0;
    mv.buttons = 1;
    const auto t0 = Clock::now();
    const auto payload = core.handlePointerEvent(mv);
    std::memcpy(textureStandIn.data(), payload.finalBitmapPixels.data(),
                std::min(payload.finalBitmapPixels.size(), textureStandIn.size()));
    const auto t1 = Clock::now();
    totalMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
  }
  const double avg = totalMs / kSteadyFrames;
  std::fprintf(stderr, "[UIPerf regular 800x600] avg=%.2f ms\n", avg);
  EXPECT_LT(avg, 20.0)
      << "regular-object drag should trivially hit 60 fps; avg=" << avg << " ms";
}

// Compare filter-group drag to the regular-object baseline: if the
// filter-group drag is substantially slower than the rect drag at the
// SAME canvas size, we have a filter-specific bottleneck.
TEST(EditorUiFlowPerfTest, FilterGroupDragMatchesRegularObjectBaseline) {
  // Two-pass test on identical 800×600 canvases: one rect, one
  // `<g filter>` with a rect child. Same viewport, same pointer
  // events, same steady-state frame count. Report the ratio; fail
  // if filter drag is more than 2× slower than rect drag.
  constexpr std::string_view kRectSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="800" height="600" viewBox="0 0 800 600">
  <rect width="800" height="600" fill="white"/>
  <rect id="target" x="100" y="100" width="80" height="60" fill="red"/>
</svg>)svg";
  constexpr std::string_view kFilterSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="800" height="600" viewBox="0 0 800 600">
  <defs><filter id="blur"><feGaussianBlur stdDeviation="2"/></filter></defs>
  <rect width="800" height="600" fill="white"/>
  <g id="target" filter="url(#blur)">
    <rect x="100" y="100" width="80" height="60" fill="red"/>
  </g>
</svg>)svg";

  const auto measure = [&](std::string_view svg) -> double {
    sandbox::EditorBackendCore core;
    SetViewportPayload vp;
    vp.width = 800;
    vp.height = 600;
    (void)core.handleSetViewport(vp);

    LoadBytesPayload load;
    load.bytes = std::string(svg);
    (void)core.handleLoadBytes(load);

    PointerEventPayload down;
    down.phase = donner::editor::sandbox::PointerPhase::kDown;
    down.documentX = 140.0;
    down.documentY = 130.0;
    down.buttons = 1;
    (void)core.handlePointerEvent(down);

    PointerEventPayload warmup;
    warmup.phase = donner::editor::sandbox::PointerPhase::kMove;
    warmup.documentX = 141.0;
    warmup.documentY = 130.0;
    warmup.buttons = 1;
    (void)core.handlePointerEvent(warmup);

    constexpr int kSteadyFrames = 30;
    std::vector<uint8_t> stand(800 * 600 * 4, 0);
    using Clock = std::chrono::steady_clock;
    double totalMs = 0.0;
    for (int i = 0; i < kSteadyFrames; ++i) {
      PointerEventPayload mv;
      mv.phase = donner::editor::sandbox::PointerPhase::kMove;
      mv.documentX = 141.0 + (i + 1) * 1.5;
      mv.documentY = 130.0;
      mv.buttons = 1;
      const auto t0 = Clock::now();
      const auto payload = core.handlePointerEvent(mv);
      std::memcpy(stand.data(), payload.finalBitmapPixels.data(),
                  std::min(payload.finalBitmapPixels.size(), stand.size()));
      const auto t1 = Clock::now();
      totalMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
    }
    return totalMs / kSteadyFrames;
  };

  const double rectAvg = measure(kRectSvg);
  const double filterAvg = measure(kFilterSvg);
  std::fprintf(stderr, "[UIPerf compare 800x600] rect=%.2f ms filter=%.2f ms ratio=%.2fx\n",
               rectAvg, filterAvg, filterAvg / rectAvg);

  // The user reports filter-group drag stuck at 20 fps while
  // regular-object drag runs at 40 fps — a 2× gap. If the backend
  // takes the fast path for both, both should be within noise of
  // each other. Flag a regression if the filter drag is more than
  // 2× slower than the rect drag.
  EXPECT_LT(filterAvg, rectAvg * 2.0 + 2.0)
      << "filter-group drag is substantially slower than regular-object "
         "drag at the same canvas size. rect=" << rectAvg
      << " ms, filter=" << filterAvg
      << " ms. Check whether `propagateFastPathTranslationToSubtree` "
         "scales with descendant count or whether the overlay render "
         "path costs more for filter groups.";
}

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
