#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/EditorSampleCatalog.h"
#include "donner/editor/tests/AsyncTestPolling.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/tests/MockRendererInterface.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "donner/svg/resources/FontCatalog.h"
#include "donner/svg/resources/FontCatalogTypes.h"
#include "donner/svg/resources/FontManager.h"

namespace donner::editor {
namespace {

using namespace std::chrono_literals;
using svg::test::RgbaEq;
using testing::ByMove;
using testing::NiceMock;
using testing::Return;
using tests::PollForResult;
using tests::WaitUntil;

struct BlockingSnapshotState {
  std::atomic<bool> entered{false};
  /// Set by the test once its assertions are done; until then a snapshot can only end by cancel.
  std::atomic<bool> released{false};
  /// Whether an interruptible snapshot ended because the worker cancelled it.
  std::atomic<bool> cancelled{false};
};

/// Holds every snapshot until the worker cancels it or the test releases it, so the test observes
/// preemption by ordering rather than by how long the readback would otherwise have taken.
class BlockingSnapshotRenderer : public svg::tests::MockRendererInterface {
public:
  explicit BlockingSnapshotRenderer(std::shared_ptr<BlockingSnapshotState> state)
      : state_(std::move(state)) {}

  svg::RendererBitmap takeSnapshot() const override {
    state_->entered.store(true, std::memory_order_release);
    WaitUntil([&] { return state_->released.load(std::memory_order_acquire); });
    return makeDummyBitmap();
  }

  svg::RendererBitmap takeSnapshotInterruptibly(
      const std::function<bool()>& shouldCancel) const override {
    state_->entered.store(true, std::memory_order_release);
    WaitUntil([&] {
      if (shouldCancel && shouldCancel()) {
        state_->cancelled.store(true, std::memory_order_release);
        return true;
      }
      return state_->released.load(std::memory_order_acquire);
    });
    return state_->cancelled.load(std::memory_order_acquire) ? svg::RendererBitmap{}
                                                             : makeDummyBitmap();
  }

private:
  std::shared_ptr<BlockingSnapshotState> state_;
};

/// Releases a \ref BlockingSnapshotState when the test ends, including on an early `ASSERT`, so
/// the worker can join. Declare it after the `AsyncRenderer` so it runs first.
class ScopedSnapshotRelease {
public:
  explicit ScopedSnapshotRelease(std::shared_ptr<BlockingSnapshotState> state)
      : state_(std::move(state)) {}
  ~ScopedSnapshotRelease() { state_->released.store(true, std::memory_order_release); }

  ScopedSnapshotRelease(const ScopedSnapshotRelease&) = delete;
  ScopedSnapshotRelease& operator=(const ScopedSnapshotRelease&) = delete;

private:
  std::shared_ptr<BlockingSnapshotState> state_;
};

/// Cancels any thumbnail work still running when the test ends, so an early `ASSERT` does not leave
/// teardown waiting out a simulated delay. Declare it after the `AsyncRenderer` so it runs first.
class ScopedThumbnailCancel {
public:
  explicit ScopedThumbnailCancel(AsyncRenderer& renderer) : renderer_(renderer) {}
  ~ScopedThumbnailCancel() { renderer_.cancelSampleThumbnailWork(); }

  ScopedThumbnailCancel(const ScopedThumbnailCancel&) = delete;
  ScopedThumbnailCancel& operator=(const ScopedThumbnailCancel&) = delete;

private:
  AsyncRenderer& renderer_;
};

class ScopedDefaultFontProvider {
public:
  explicit ScopedDefaultFontProvider(const svg::FontFamilyProvider* provider)
      : previous_(svg::FontManager::DefaultFontProvider()) {
    svg::FontManager::SetDefaultFontProvider(provider);
  }

  ~ScopedDefaultFontProvider() { svg::FontManager::SetDefaultFontProvider(previous_); }

private:
  const svg::FontFamilyProvider* previous_ = nullptr;
};

constexpr std::string_view kRedSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 24">
  <rect width="32" height="24" fill="#e02020"/>
</svg>
)svg";

constexpr std::string_view kBlueSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 24">
  <rect width="32" height="24" fill="#2050e0"/>
</svg>
)svg";

constexpr std::string_view kFilteredSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 24">
  <defs><filter id="f"><feFlood flood-color="#20e080"/></filter></defs>
  <rect width="32" height="24" fill="#e02020" filter="url(#f)"/>
</svg>
)svg";

std::optional<SampleThumbnailRenderResult> WaitForThumbnailResult(AsyncRenderer& renderer) {
  return PollForResult([&] { return renderer.pollSampleThumbnailResult(); });
}

std::array<std::uint8_t, 4> PixelAt(const svg::RendererBitmap& bitmap, int x, int y) {
  const std::size_t offset =
      static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
  return {bitmap.pixels[offset], bitmap.pixels[offset + 1u], bitmap.pixels[offset + 2u],
          bitmap.pixels[offset + 3u]};
}

bool PixelNear(const svg::RendererBitmap& bitmap, int x, int y, const std::array<int, 3>& expected,
               int tolerance) {
  const std::array<std::uint8_t, 4> pixel = PixelAt(bitmap, x, y);
  return pixel[3] > 0u && std::abs(static_cast<int>(pixel[0]) - expected[0]) <= tolerance &&
         std::abs(static_cast<int>(pixel[1]) - expected[1]) <= tolerance &&
         std::abs(static_cast<int>(pixel[2]) - expected[2]) <= tolerance;
}

std::size_t CountPixelsNear(const svg::RendererBitmap& bitmap, const std::array<int, 3>& expected,
                            int tolerance) {
  std::size_t count = 0u;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      count += PixelNear(bitmap, x, y, expected, tolerance) ? 1u : 0u;
    }
  }
  return count;
}

std::size_t CountMintPixels(const svg::RendererBitmap& bitmap) {
  std::size_t count = 0u;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const std::array<std::uint8_t, 4> pixel = PixelAt(bitmap, x, y);
      count +=
          pixel[3] > 0u && pixel[0] > 60u && pixel[1] > pixel[0] + 25u && pixel[1] > pixel[2] + 5u
              ? 1u
              : 0u;
    }
  }
  return count;
}

SampleThumbnailRenderRequest ThumbnailRequest(std::uint64_t key, std::string_view source,
                                              svg::RendererInterface& nativeRenderer,
                                              Vector2i dimensions = Vector2i(48, 36)) {
  return SampleThumbnailRenderRequest{
      .key = key,
      .source = std::string(source),
      .dimensions = dimensions,
      .nativeRenderer = &nativeRenderer,
  };
}

TEST(SampleThumbnailAsyncTest, MainDocumentPreemptsThumbnailDuringSnapshotReadback) {
  NiceMock<svg::tests::MockRendererInterface> thumbnailRoot;
  const auto snapshotState = std::make_shared<BlockingSnapshotState>();
  std::unique_ptr<svg::RendererInterface> blockingSnapshotRenderer =
      std::make_unique<NiceMock<BlockingSnapshotRenderer>>(snapshotState);
  EXPECT_CALL(thumbnailRoot, createOffscreenInstance())
      .WillOnce(Return(ByMove(std::move(blockingSnapshotRenderer))));

  // The document and its renderer outlive the worker that renders them, so a failed ASSERT below
  // tears the worker down first.
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = svg::parser::SVGParser::ParseSVG(kBlueSvg, warnings);
  ASSERT_FALSE(parsed.hasError());
  svg::SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(64, 48);
  svg::Renderer documentRenderer;

  AsyncRenderer renderer;
  const ScopedSnapshotRelease releaseSnapshotAtEnd(snapshotState);
  ASSERT_TRUE(renderer.requestSampleThumbnail(ThumbnailRequest(51u, kRedSvg, thumbnailRoot)));
  ASSERT_TRUE(WaitUntil([&] { return snapshotState->entered.load(std::memory_order_acquire); }))
      << "Expected the thumbnail to reach its snapshot/readback phase";

  RenderRequest mainRequest(documentRenderer, document);
  mainRequest.version = 2u;
  mainRequest.documentGeneration = 2u;
  mainRequest.rasterViewport = EditorRasterViewport{
      .documentRect = Box2d::FromXYWH(0.0, 0.0, 64.0, 48.0),
      .outputSizePx = Vector2i(64, 48),
      .semanticCanvasSizePx = Vector2i(64, 48),
      .outputFromDocument = Transform2d(),
  };

  // The readback never finishes on its own, so the main result can only arrive by preempting it.
  renderer.requestRender(mainRequest);
  const std::optional<RenderResult> mainResult =
      PollForResult([&] { return renderer.pollResult(); });

  ASSERT_TRUE(mainResult.has_value())
      << "Main-document work must not wait behind a low-priority GPU map/readback";
  EXPECT_EQ(mainResult->version, 2u);
  EXPECT_TRUE(snapshotState->cancelled.load(std::memory_order_acquire))
      << "The main render must cancel the held readback, not wait for it";
  EXPECT_FALSE(snapshotState->released.load(std::memory_order_acquire));
  std::optional<SampleThumbnailRenderResult> cancelled = WaitForThumbnailResult(renderer);
  ASSERT_TRUE(cancelled.has_value());
  EXPECT_EQ(cancelled->key, 51u);
  EXPECT_EQ(cancelled->outcome, SampleThumbnailRenderOutcome::Cancelled);
}

struct BlockingFontLoadState {
  std::atomic<bool> entered{false};
  /// Set by the test once its assertions are done; until then a load can only end by interruption.
  std::atomic<bool> released{false};
  /// Whether the load ended because the loading thread asked it to stop.
  std::atomic<bool> interrupted{false};
};

/// Holds every load of its one family until the loading thread interrupts it or the test releases
/// it, standing in for a provider that rebuilds a large system font.
class BlockingFontProvider : public svg::FontFamilyProvider {
public:
  static constexpr std::string_view kFamily = "Blocking Preview Family";

  explicit BlockingFontProvider(std::shared_ptr<BlockingFontLoadState> state)
      : state_(std::move(state)) {}

  std::vector<svg::FontFamilyInfo> families() const override {
    return {svg::FontFamilyInfo{std::string(kFamily), svg::FontSource::System,
                                svg::FontCategory::Unknown}};
  }

  bool hasFamily(std::string_view family) const override { return family == kFamily; }

  std::vector<uint8_t> loadFamilyData(std::string_view /*family*/,
                                      const svg::FontFaceRequest& /*request*/) const override {
    state_->entered.store(true, std::memory_order_release);
    WaitUntil([&] {
      if (svg::ScopedFontLoadInterruption::Requested()) {
        state_->interrupted.store(true, std::memory_order_release);
        return true;
      }
      return state_->released.load(std::memory_order_acquire);
    });
    return {};
  }

private:
  std::shared_ptr<BlockingFontLoadState> state_;
};

/// Releases a \ref BlockingFontLoadState when the test ends, including on an early `ASSERT`, so
/// the worker can join. Declare it after the `AsyncRenderer` so it runs first.
class ScopedFontLoadRelease {
public:
  explicit ScopedFontLoadRelease(std::shared_ptr<BlockingFontLoadState> state)
      : state_(std::move(state)) {}
  ~ScopedFontLoadRelease() { state_->released.store(true, std::memory_order_release); }

  ScopedFontLoadRelease(const ScopedFontLoadRelease&) = delete;
  ScopedFontLoadRelease& operator=(const ScopedFontLoadRelease&) = delete;

private:
  std::shared_ptr<BlockingFontLoadState> state_;
};

TEST(SampleThumbnailAsyncTest, MainDocumentPreemptsThumbnailDuringProviderFontLoad) {
  const auto fontLoadState = std::make_shared<BlockingFontLoadState>();
  const BlockingFontProvider provider(fontLoadState);
  const ScopedDefaultFontProvider installProvider(&provider);

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = svg::parser::SVGParser::ParseSVG(kBlueSvg, warnings);
  ASSERT_FALSE(parsed.hasError());
  svg::SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(64, 48);
  svg::Renderer documentRenderer;
  svg::Renderer thumbnailRoot;

  AsyncRenderer renderer;
  const ScopedFontLoadRelease releaseFontLoadAtEnd(fontLoadState);
  SampleThumbnailRenderRequest preview =
      ThumbnailRequest(52u,
                       R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 24">
        <text x="2" y="18" font-family="Blocking Preview Family">Aa</text></svg>)svg",
                       thumbnailRoot);
  preview.kind = AuxiliaryPreviewKind::FontFamily;
  ASSERT_TRUE(renderer.requestSampleThumbnail(std::move(preview)));
  ASSERT_TRUE(WaitUntil([&] { return fontLoadState->entered.load(std::memory_order_acquire); }))
      << "Expected the thumbnail to reach its provider font load";

  RenderRequest mainRequest(documentRenderer, document);
  mainRequest.version = 2u;
  mainRequest.documentGeneration = 2u;
  mainRequest.rasterViewport = EditorRasterViewport{
      .documentRect = Box2d::FromXYWH(0.0, 0.0, 64.0, 48.0),
      .outputSizePx = Vector2i(64, 48),
      .semanticCanvasSizePx = Vector2i(64, 48),
      .outputFromDocument = Transform2d(),
  };

  // The font load never finishes on its own, so the main result can only arrive by interrupting it.
  renderer.requestRender(mainRequest);
  const std::optional<RenderResult> mainResult =
      PollForResult([&] { return renderer.pollResult(); });

  ASSERT_TRUE(mainResult.has_value())
      << "Main-document work must not wait behind a preview's provider font load";
  EXPECT_EQ(mainResult->version, 2u);
  EXPECT_TRUE(fontLoadState->interrupted.load(std::memory_order_acquire))
      << "The main render must interrupt the preview's font load, not wait for it";
  EXPECT_FALSE(fontLoadState->released.load(std::memory_order_acquire));
  std::optional<SampleThumbnailRenderResult> cancelled = WaitForThumbnailResult(renderer);
  ASSERT_TRUE(cancelled.has_value());
  EXPECT_EQ(cancelled->key, 52u);
  EXPECT_EQ(cancelled->outcome, SampleThumbnailRenderOutcome::Cancelled);
}

TEST(SampleThumbnailAsyncTest, FirstOffscreenCreationCompletesBeforeForegroundHandoff) {
  struct CreationGate {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
  };
  const auto gate = std::make_shared<CreationGate>();
  NiceMock<svg::tests::MockRendererInterface> thumbnailRoot;
  EXPECT_CALL(thumbnailRoot, createOffscreenInstance())
      .WillOnce([gate]() -> std::unique_ptr<svg::RendererInterface> {
        gate->entered.store(true, std::memory_order_release);
        while (!gate->release.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(1ms);
        }
        return std::make_unique<svg::Renderer>();
      });

  // The document and its renderer outlive the worker that renders them.
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = svg::parser::SVGParser::ParseSVG(kBlueSvg, warnings);
  ASSERT_FALSE(parsed.hasError());
  svg::SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(64, 48);
  svg::Renderer documentRenderer;
  AsyncRenderer renderer;
  ASSERT_TRUE(renderer.requestSampleThumbnail(ThumbnailRequest(52u, kRedSvg, thumbnailRoot)));
  ASSERT_TRUE(WaitUntil([&] { return gate->entered.load(std::memory_order_acquire); }));

  RenderRequest mainRequest(documentRenderer, document);
  mainRequest.version = 3u;
  mainRequest.documentGeneration = 3u;
  mainRequest.rasterViewport = EditorRasterViewport{
      .documentRect = Box2d::FromXYWH(0.0, 0.0, 64.0, 48.0),
      .outputSizePx = Vector2i(64, 48),
      .semanticCanvasSizePx = Vector2i(64, 48),
      .outputFromDocument = Transform2d(),
  };

  renderer.cancelSampleThumbnailWork();
  renderer.requestRender(mainRequest);
  gate->release.store(true, std::memory_order_release);
  ASSERT_TRUE(WaitUntil([&] { return renderer.sampleThumbnailRenderStats().completed == 1u; }));

  const SampleThumbnailRenderStats stats = renderer.sampleThumbnailRenderStats();
  EXPECT_EQ(stats.foregroundHandoffWaits, 1u);
  EXPECT_TRUE(stats.firstAttemptCompleted);

  std::optional<RenderResult> mainResult;
  ASSERT_TRUE(WaitUntil([&] {
    mainResult = renderer.pollResult();
    return mainResult.has_value();
  }));
  EXPECT_EQ(mainResult->version, 3u);
}

TEST(SampleThumbnailAsyncTest, ShutdownRejectsBothPriorityLanesWithoutResurrectingWorker) {
  svg::Renderer thumbnailRoot;
  AsyncRenderer renderer;
  renderer.shutdown();

  EXPECT_FALSE(renderer.requestSampleThumbnail(ThumbnailRequest(61u, kRedSvg, thumbnailRoot)));

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = svg::parser::SVGParser::ParseSVG(kBlueSvg, warnings);
  ASSERT_FALSE(parsed.hasError());
  svg::SVGDocument document = std::move(parsed.result());
  svg::Renderer documentRenderer;
  RenderRequest mainRequest(documentRenderer, document);
  mainRequest.version = 3u;
  mainRequest.documentGeneration = 3u;
  renderer.requestRender(mainRequest);

  EXPECT_FALSE(renderer.isBusy());
  EXPECT_FALSE(renderer.hasRenderInFlightForTesting());
  EXPECT_FALSE(renderer.pollResult().has_value());
  EXPECT_FALSE(renderer.pollSampleThumbnailResult().has_value());
  EXPECT_EQ(renderer.sampleThumbnailRenderStats().requested, 0u);
}

TEST(SampleThumbnailAsyncTest, ShutdownCancelsActiveThumbnailJoinsPromptlyAndSuppressesWake) {
  svg::Renderer thumbnailRoot;
  // Callback state outlives the worker that calls into it.
  std::atomic<int> wakeCount{0};
  AsyncRenderer renderer;
  renderer.setWakeCallback([&] { wakeCount.fetch_add(1, std::memory_order_relaxed); });
  renderer.setSampleThumbnailRenderDelayForTesting(tests::kUnelapsedDelay);
  ASSERT_TRUE(renderer.requestSampleThumbnail(ThumbnailRequest(62u, kRedSvg, thumbnailRoot)));
  ASSERT_TRUE(WaitUntil([&] { return renderer.sampleThumbnailRenderStats().active; }));

  // The active thumbnail would sleep for an hour, so shutdown returning at all shows it cancelled
  // that work instead of waiting for it. Shutdown runs on another thread so a hang is reported here
  // when the stall guard expires.
  auto shutdown = std::async(std::launch::async, [&] { renderer.shutdown(); });
  // Declared after the future so it runs first: if shutdown hangs, cancelling the thumbnail lets it
  // finish and the case ends with the assertion below instead of a test timeout.
  const ScopedThumbnailCancel cancelThumbnailIfShutdownHangs(renderer);
  ASSERT_EQ(shutdown.wait_until(tests::HangGuard().deadline()), std::future_status::ready)
      << "shutdown() waited for the dispensable thumbnail work instead of cancelling it";

  EXPECT_EQ(wakeCount.load(std::memory_order_relaxed), 0)
      << "No thumbnail completion wake may escape after shutdown begins";
  EXPECT_FALSE(renderer.isBusy());
  EXPECT_FALSE(renderer.hasRenderInFlightForTesting());
  const SampleThumbnailRenderStats stats = renderer.sampleThumbnailRenderStats();
  EXPECT_FALSE(stats.pending);
  EXPECT_FALSE(stats.active);
  EXPECT_FALSE(stats.resultReady);
}

TEST(SampleThumbnailAsyncTest, RendersSourceDependentBitmapsThroughOneBoundedWorkerSlot) {
  svg::Renderer thumbnailRoot;
  AsyncRenderer renderer;

  ASSERT_TRUE(renderer.requestSampleThumbnail(ThumbnailRequest(11u, kRedSvg, thumbnailRoot)));
  EXPECT_FALSE(renderer.requestSampleThumbnail(ThumbnailRequest(12u, kBlueSvg, thumbnailRoot)))
      << "Only one low-priority thumbnail may be pending, active, or waiting for pickup";

  std::optional<SampleThumbnailRenderResult> red = WaitForThumbnailResult(renderer);
  ASSERT_TRUE(red.has_value());
  ASSERT_EQ(red->key, 11u);
  ASSERT_EQ(red->outcome, SampleThumbnailRenderOutcome::Rendered);
  ASSERT_EQ(red->bitmap.dimensions, Vector2i(48, 36));
  EXPECT_THAT(PixelAt(red->bitmap, 24, 18), RgbaEq(0xe0, 0x20, 0x20, 0xff));

  ASSERT_TRUE(renderer.requestSampleThumbnail(ThumbnailRequest(12u, kBlueSvg, thumbnailRoot)));
  std::optional<SampleThumbnailRenderResult> blue = WaitForThumbnailResult(renderer);
  ASSERT_TRUE(blue.has_value());
  ASSERT_EQ(blue->key, 12u);
  ASSERT_EQ(blue->outcome, SampleThumbnailRenderOutcome::Rendered);
  ASSERT_EQ(blue->bitmap.dimensions, Vector2i(48, 36));
  EXPECT_THAT(PixelAt(blue->bitmap, 24, 18), RgbaEq(0x20, 0x50, 0xe0, 0xff));
  EXPECT_NE(red->bitmap.pixels, blue->bitmap.pixels)
      << "Catalog thumbnails must be rendered from their SVG source, not a shared placeholder";

  const SampleThumbnailRenderStats stats = renderer.sampleThumbnailRenderStats();
  EXPECT_EQ(stats.requested, 2u);
  EXPECT_EQ(stats.started, 2u);
  EXPECT_EQ(stats.completed, 2u);
  EXPECT_EQ(stats.rendered, 2u);
  EXPECT_EQ(stats.readbackCount, thumbnailRoot.supportsTextureSnapshotCompositing() ? 2u : 0u);
  EXPECT_EQ(stats.offscreenRendererCreations, 1u)
      << "The worker must lazily create and then reuse one offscreen renderer";
  EXPECT_FALSE(stats.pending);
  EXPECT_FALSE(stats.active);
  EXPECT_FALSE(stats.resultReady);
}

TEST(SampleThumbnailAsyncTest, ReusedOffscreenRendererStartsFreshFilterPreparationFrames) {
  svg::Renderer thumbnailRoot;
  svg::RendererFilterPreparationBudget* budget = thumbnailRoot.filterPreparationBudget();
  ASSERT_NE(budget, nullptr);
  svg::RendererFilterPreparationBudget::Limits limits;
  limits.graphs = 1u;
  budget->setLimitsForTesting(limits);

  AsyncRenderer renderer;
  ASSERT_TRUE(renderer.requestSampleThumbnail(ThumbnailRequest(21u, kFilteredSvg, thumbnailRoot)));
  std::optional<SampleThumbnailRenderResult> first = WaitForThumbnailResult(renderer);
  ASSERT_TRUE(first.has_value());
  ASSERT_EQ(first->outcome, SampleThumbnailRenderOutcome::Rendered);
  EXPECT_EQ(budget->attempts(), 1u);
  EXPECT_FALSE(budget->rejected());

  ASSERT_TRUE(renderer.requestSampleThumbnail(ThumbnailRequest(22u, kFilteredSvg, thumbnailRoot)));
  std::optional<SampleThumbnailRenderResult> second = WaitForThumbnailResult(renderer);
  ASSERT_TRUE(second.has_value());
  ASSERT_EQ(second->outcome, SampleThumbnailRenderOutcome::Rendered);
  EXPECT_EQ(budget->attempts(), 1u);
  EXPECT_FALSE(budget->rejected());
}

TEST(SampleThumbnailAsyncTest, TextAndStyleThumbnailContainsDonnerRenderedGlyphPixels) {
  const EditorSample* sample = FindEditorSample("text-style");
  ASSERT_NE(sample, nullptr);

  svg::FontCatalog fontCatalog;
  const ScopedDefaultFontProvider scopedFonts(&fontCatalog);
  ASSERT_TRUE(fontCatalog.hasFamily("Inter"));
  svg::Renderer thumbnailRoot;
  AsyncRenderer renderer;
  ASSERT_TRUE(renderer.requestSampleThumbnail(
      ThumbnailRequest(21u, sample->source, thumbnailRoot, Vector2i(192, 120))));

  std::optional<SampleThumbnailRenderResult> result = WaitForThumbnailResult(renderer);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->outcome, SampleThumbnailRenderOutcome::Rendered);
  ASSERT_FALSE(result->bitmap.empty());

  // These colors occur in the actual heading/body glyphs. The dark background and gold rule still
  // render when text support is accidentally omitted, so pin both glyph colors explicitly.
  EXPECT_GT(CountPixelsNear(result->bitmap, {0xf8, 0xf9, 0xfa}, /*tolerance=*/12), 40u);
  EXPECT_GT(CountMintPixels(result->bitmap), 20u)
      << "nearMint=" << CountPixelsNear(result->bitmap, {0x9f, 0xe3, 0xc0}, /*tolerance=*/48);
}

TEST(SampleThumbnailAsyncTest, MainDocumentRenderPreemptsAndStaysAheadOfLowPriorityWork) {
  svg::Renderer thumbnailRoot;
  // The document and its renderer outlive the worker that renders them.
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = svg::parser::SVGParser::ParseSVG(kBlueSvg, warnings);
  ASSERT_FALSE(parsed.hasError());
  svg::SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(64, 48);
  svg::Renderer documentRenderer;
  AsyncRenderer renderer;
  // If an assertion below ends the case while the first thumbnail still waits out its hour-long
  // delay, cancel it so teardown does not wait for it.
  const ScopedThumbnailCancel cancelThumbnailAtEnd(renderer);
  // The first thumbnail can only finish by being preempted, however long the host takes to post
  // the main request.
  renderer.setSampleThumbnailRenderDelayForTesting(tests::kUnelapsedDelay);
  ASSERT_TRUE(renderer.requestSampleThumbnail(ThumbnailRequest(31u, kRedSvg, thumbnailRoot)));
  ASSERT_TRUE(WaitUntil([&] { return renderer.sampleThumbnailRenderStats().active; }))
      << "Expected the first thumbnail to enter the worker before posting main-document work";

  RenderRequest mainRequest(documentRenderer, document);
  mainRequest.version = 1u;
  mainRequest.documentGeneration = 1u;
  mainRequest.rasterViewport = EditorRasterViewport{
      .documentRect = Box2d::FromXYWH(0.0, 0.0, 64.0, 48.0),
      .outputSizePx = Vector2i(64, 48),
      .semanticCanvasSizePx = Vector2i(64, 48),
      .outputFromDocument = Transform2d(),
  };
  renderer.requestRender(mainRequest);

  std::optional<SampleThumbnailRenderResult> cancelled = WaitForThumbnailResult(renderer);
  ASSERT_TRUE(cancelled.has_value());
  EXPECT_EQ(cancelled->key, 31u);
  EXPECT_EQ(cancelled->outcome, SampleThumbnailRenderOutcome::Cancelled);

  // The low-priority slot may accept the next card while the main request is active, but it must
  // remain queued until the main result is handed to the UI.
  renderer.setSampleThumbnailRenderDelayForTesting(0ms);
  ASSERT_TRUE(renderer.requestSampleThumbnail(ThumbnailRequest(32u, kRedSvg, thumbnailRoot)));

  bool mainResultObserved = false;
  bool secondThumbnailObserved = false;
  for (const tests::HangGuard guard; guard.active() && !secondThumbnailObserved;) {
    if (std::optional<SampleThumbnailRenderResult> thumbnail =
            renderer.pollSampleThumbnailResult()) {
      EXPECT_TRUE(mainResultObserved)
          << "Low-priority thumbnail result overtook the main document render";
      EXPECT_EQ(thumbnail->key, 32u);
      EXPECT_EQ(thumbnail->outcome, SampleThumbnailRenderOutcome::Rendered);
      secondThumbnailObserved = true;
    }
    if (std::optional<RenderResult> main = renderer.pollResult()) {
      EXPECT_EQ(main->version, 1u);
      mainResultObserved = true;
    }
    std::this_thread::sleep_for(1ms);
  }

  EXPECT_TRUE(mainResultObserved);
  EXPECT_TRUE(secondThumbnailObserved);
  EXPECT_GE(renderer.sampleThumbnailRenderStats().cancelled, 1u);
}

TEST(SampleThumbnailAsyncTest, CompletionWakesOnceThenWorkerRemainsQuiescent) {
  svg::Renderer thumbnailRoot;
  // Callback state outlives the worker that calls into it, including through an early ASSERT.
  std::atomic<int> wakeCount{0};
  std::atomic<int> idleCycles{0};
  // The document and its renderer outlive the worker that renders them.
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = svg::parser::SVGParser::ParseSVG(kBlueSvg, warnings);
  ASSERT_FALSE(parsed.hasError());
  svg::SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(32, 24);
  svg::Renderer documentRenderer;
  AsyncRenderer renderer;
  renderer.setWakeCallback([&] { wakeCount.fetch_add(1, std::memory_order_relaxed); });
  // Count the worker's idle cycles: with an idle poll installed, the idle worker wakes on its own
  // about once a second, runs the poll and parks again, publishing nothing.
  renderer.setIdleMaintenance([&] { idleCycles.fetch_add(1, std::memory_order_acq_rel); },
                              [] { return false; });

  ASSERT_TRUE(renderer.requestSampleThumbnail(ThumbnailRequest(41u, kRedSvg, thumbnailRoot)));
  ASSERT_TRUE(WaitForThumbnailResult(renderer).has_value());
  ASSERT_TRUE(WaitUntil([&] { return wakeCount.load(std::memory_order_relaxed) > 0; }));
  EXPECT_EQ(wakeCount.load(std::memory_order_relaxed), 1)
      << "Each completed thumbnail must issue exactly one host wake";

  // Drive the worker back through its idle wait with one main render, which wakes the host once.
  // Any further wake came from the finished thumbnail. Compositing stays off so no deferred warmup
  // adds a wake of its own.
  renderer.setCompositedRenderingMode(CompositedRenderingMode::Off);
  RenderRequest mainRequest(documentRenderer, document);
  mainRequest.version = 1u;
  mainRequest.documentGeneration = 1u;
  renderer.requestRender(mainRequest);
  ASSERT_TRUE(PollForResult([&] { return renderer.pollResult(); }).has_value());
  ASSERT_TRUE(WaitUntil([&] { return wakeCount.load(std::memory_order_relaxed) >= 2; }));
  ASSERT_TRUE(renderer.waitUntilNoRenderInFlightForTesting(tests::HangGuard().deadline()));
  // Let the worker park and wake on its own twice with nothing to do. A wake source the thumbnail
  // left behind, whether tied to worker cycles or a delay, fires during those idle cycles.
  const int idleCyclesBefore = idleCycles.load(std::memory_order_acquire);
  ASSERT_TRUE(WaitUntil([&] {
    return idleCycles.load(std::memory_order_acquire) >= idleCyclesBefore + 2;
  })) << "the idle worker never cycled";
  EXPECT_EQ(wakeCount.load(std::memory_order_relaxed), 2)
      << "Completed thumbnail work must not install a recurring frame/update source";
  const SampleThumbnailRenderStats stats = renderer.sampleThumbnailRenderStats();
  EXPECT_FALSE(stats.pending);
  EXPECT_FALSE(stats.active);
  EXPECT_FALSE(stats.resultReady);
}

TEST(SampleThumbnailAsyncTest, FontAdoptionWaitsForHeldAuxiliaryResult) {
  svg::Renderer thumbnailRoot;
  AsyncRenderer renderer;
  auto request = ThumbnailRequest(91u, kRedSvg, thumbnailRoot);
  request.taskGeneration = 8;
  request.fontWakeRevision = 12;
  ASSERT_EQ(renderer.requestSampleThumbnail(std::move(request)), true);
  EXPECT_EQ(renderer.isFontResourceAdoptionSafe(), false);
  ASSERT_EQ(WaitUntil([&] { return renderer.sampleThumbnailRenderStats().resultReady; }), true);
  EXPECT_EQ(renderer.isFontResourceAdoptionSafe(), false);
  const auto result = renderer.pollSampleThumbnailResult();
  ASSERT_EQ(result.has_value(), true);
  EXPECT_EQ(result->taskGeneration, 8u);
  EXPECT_EQ(result->fontWakeRevision, 12u);
  EXPECT_EQ(renderer.isFontResourceAdoptionSafe(), true);
}

TEST(SampleThumbnailAsyncTest, FontAdoptionWaitsForHeldDocumentResultAndPreservesItsEpoch) {
  svg::Renderer root;
  AsyncRenderer renderer;
  renderer.setCompositedRenderingMode(CompositedRenderingMode::Off);
  ParseWarningSink warnings;
  auto parsed = svg::parser::SVGParser::ParseSVG(kRedSvg, warnings);
  ASSERT_EQ(parsed.hasResult(), true);
  auto document = std::move(parsed.result());
  document.setCanvasSize(32, 24);
  RenderRequest request(root, document);
  request.version = 7;
  request.documentGeneration = 2;
  request.fontResourceRevision = 3;
  renderer.requestRender(request);
  ASSERT_EQ(renderer.waitUntilNoRenderInFlightForTesting(tests::HangGuard().deadline()), true);
  EXPECT_EQ(renderer.isFontResourceAdoptionSafe(), false);
  const auto result = renderer.pollResult();
  ASSERT_EQ(result.has_value(), true);
  EXPECT_EQ(result->version, 7u);
  EXPECT_EQ(result->documentGeneration, 2u);
  EXPECT_EQ(result->fontResourceRevision, 3u);
  EXPECT_EQ(renderer.isFontResourceAdoptionSafe(), true);
}

}  // namespace
}  // namespace donner::editor
