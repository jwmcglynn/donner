#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/EditorSampleCatalog.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/tests/MockRendererInterface.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "donner/svg/resources/FontCatalog.h"
#include "donner/svg/resources/FontManager.h"

namespace donner::editor {
namespace {

using namespace std::chrono_literals;
using svg::test::RgbaEq;
using testing::ByMove;
using testing::NiceMock;
using testing::Return;

struct BlockingSnapshotState {
  std::atomic<bool> entered{false};
};

class BlockingSnapshotRenderer : public svg::tests::MockRendererInterface {
public:
  explicit BlockingSnapshotRenderer(std::shared_ptr<BlockingSnapshotState> state)
      : state_(std::move(state)) {}

  svg::RendererBitmap takeSnapshot() const override {
    state_->entered.store(true, std::memory_order_release);
    std::this_thread::sleep_for(1500ms);
    return makeDummyBitmap();
  }

  svg::RendererBitmap takeSnapshotInterruptibly(
      const std::function<bool()>& shouldCancel) const override {
    state_->entered.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + 1500ms;
    while (std::chrono::steady_clock::now() < deadline) {
      if (shouldCancel && shouldCancel()) {
        return {};
      }
      std::this_thread::sleep_for(1ms);
    }
    return makeDummyBitmap();
  }

private:
  std::shared_ptr<BlockingSnapshotState> state_;
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

std::optional<SampleThumbnailRenderResult> WaitForThumbnailResult(
    AsyncRenderer& renderer, std::chrono::seconds timeout = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (std::optional<SampleThumbnailRenderResult> result = renderer.pollSampleThumbnailResult()) {
      return result;
    }
    std::this_thread::sleep_for(1ms);
  }
  return std::nullopt;
}

template <typename Predicate>
bool WaitUntil(Predicate&& predicate, std::chrono::seconds timeout = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return false;
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

  AsyncRenderer renderer;
  ASSERT_TRUE(renderer.requestSampleThumbnail(ThumbnailRequest(51u, kRedSvg, thumbnailRoot)));
  ASSERT_TRUE(WaitUntil([&] { return snapshotState->entered.load(std::memory_order_acquire); }))
      << "Expected the thumbnail to reach its snapshot/readback phase";

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = svg::parser::SVGParser::ParseSVG(kBlueSvg, warnings);
  ASSERT_FALSE(parsed.hasError());
  svg::SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(64, 48);
  svg::Renderer documentRenderer;
  RenderRequest mainRequest(documentRenderer, document);
  mainRequest.version = 2u;
  mainRequest.documentGeneration = 2u;
  mainRequest.rasterViewport = EditorRasterViewport{
      .documentRect = Box2d::FromXYWH(0.0, 0.0, 64.0, 48.0),
      .outputSizePx = Vector2i(64, 48),
      .semanticCanvasSizePx = Vector2i(64, 48),
      .outputFromDocument = Transform2d(),
  };

  const auto requestTime = std::chrono::steady_clock::now();
  renderer.requestRender(mainRequest);
  std::optional<RenderResult> mainResult;
  const auto deadline = requestTime + 700ms;
  while (std::chrono::steady_clock::now() < deadline && !mainResult.has_value()) {
    mainResult = renderer.pollResult();
    std::this_thread::sleep_for(1ms);
  }

  ASSERT_TRUE(mainResult.has_value())
      << "Main-document work must not wait behind a low-priority GPU map/readback";
  EXPECT_EQ(mainResult->version, 2u);
  EXPECT_LT(std::chrono::steady_clock::now() - requestTime, 700ms);
  std::optional<SampleThumbnailRenderResult> cancelled = WaitForThumbnailResult(renderer);
  ASSERT_TRUE(cancelled.has_value());
  EXPECT_EQ(cancelled->key, 51u);
  EXPECT_EQ(cancelled->outcome, SampleThumbnailRenderOutcome::Cancelled);
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
  AsyncRenderer renderer;
  std::atomic<int> wakeCount{0};
  renderer.setWakeCallback([&] { wakeCount.fetch_add(1, std::memory_order_relaxed); });
  renderer.setSampleThumbnailRenderDelayForTesting(5s);
  ASSERT_TRUE(renderer.requestSampleThumbnail(ThumbnailRequest(62u, kRedSvg, thumbnailRoot)));
  ASSERT_TRUE(WaitUntil([&] { return renderer.sampleThumbnailRenderStats().active; }));

  const auto start = std::chrono::steady_clock::now();
  renderer.shutdown();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_LT(elapsed, 250ms) << "Shutdown must not wait for dispensable background thumbnail work";
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
  EXPECT_EQ(stats.offscreenRendererCreations, 1u)
      << "The worker must lazily create and then reuse one offscreen renderer";
  EXPECT_FALSE(stats.pending);
  EXPECT_FALSE(stats.active);
  EXPECT_FALSE(stats.resultReady);
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
  AsyncRenderer renderer;
  renderer.setSampleThumbnailRenderDelayForTesting(250ms);
  ASSERT_TRUE(renderer.requestSampleThumbnail(ThumbnailRequest(31u, kRedSvg, thumbnailRoot)));
  ASSERT_TRUE(WaitUntil([&] { return renderer.sampleThumbnailRenderStats().active; }))
      << "Expected the first thumbnail to enter the worker before posting main-document work";

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = svg::parser::SVGParser::ParseSVG(kBlueSvg, warnings);
  ASSERT_FALSE(parsed.hasError());
  svg::SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(64, 48);
  svg::Renderer documentRenderer;
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
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline && !secondThumbnailObserved) {
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
  AsyncRenderer renderer;
  std::atomic<int> wakeCount{0};
  renderer.setWakeCallback([&] { wakeCount.fetch_add(1, std::memory_order_relaxed); });

  ASSERT_TRUE(renderer.requestSampleThumbnail(ThumbnailRequest(41u, kRedSvg, thumbnailRoot)));
  ASSERT_TRUE(WaitForThumbnailResult(renderer).has_value());
  ASSERT_TRUE(WaitUntil([&] { return wakeCount.load(std::memory_order_relaxed) > 0; }));
  EXPECT_EQ(wakeCount.load(std::memory_order_relaxed), 1)
      << "Each completed thumbnail must issue exactly one host wake";

  std::this_thread::sleep_for(75ms);
  EXPECT_EQ(wakeCount.load(std::memory_order_relaxed), 1)
      << "Completed thumbnail work must not install a recurring frame/update source";
  const SampleThumbnailRenderStats stats = renderer.sampleThumbnailRenderStats();
  EXPECT_FALSE(stats.pending);
  EXPECT_FALSE(stats.active);
  EXPECT_FALSE(stats.resultReady);
}

}  // namespace
}  // namespace donner::editor
