/// @file
/// One document drawn by renderers on two devices. Each device keeps its own GPU residence for the
/// document, so drawing on one device neither releases nor rebuilds the other's, nothing one device
/// owns is released on another device's thread, a document may outlive a device that drew it, and
/// a device that goes stops counting against the document's geometry budget. A renderer also lets
/// go of everything it borrowed from a document when the frame that drew it ends, while the
/// document is still held, so nothing it does afterwards touches a document another thread may be
/// changing or destroying.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/RcString.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/geode/GeodeCounters.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeEmbed.h"
#include "donner/svg/renderer/geode/GeodeHandleRetirement.h"
#include "donner/svg/renderer/geode/GeodeResourceBudget.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"

namespace donner::svg {
namespace {

using testing::Eq;
using testing::Ge;
using testing::Gt;
using testing::IsFalse;
using testing::IsTrue;
using testing::NotNull;

constexpr std::string_view kShapesSvg = R"(
  <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" width="200" height="200">
    <rect id="box" x="10" y="10" width="80" height="60" fill="#c33"/>
    <circle cx="140" cy="60" r="40" fill="#3a6" stroke="#036" stroke-width="6"/>
    <path id="curve" d="M20 180 C60 110 120 210 180 130" fill="none" stroke="#639"
          stroke-width="8"/>
    <ellipse cx="100" cy="150" rx="50" ry="20" fill="#fc3" opacity="0.8"/>
  </svg>)";

/// Two text runs, so a device's glyph cache and every text element's occurrence records hold
/// residence of their own.
constexpr std::string_view kTextSvg = R"(
  <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" width="200" height="200"
       font-family="Noto Sans" font-size="32">
    <text x="10" y="60" fill="black">eeee</text>
    <text x="10" y="140" fill="#36c">donner</text>
  </svg>)";

/// One rectangle and nothing else. With no other draw to batch with, its draw is still pending when
/// anything later in the frame runs, and it is given residence only when the frame flushes it at
/// its end.
constexpr std::string_view kLoneRectSvg = R"(
  <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" width="200" height="200">
    <rect x="10" y="10" width="80" height="60" fill="#c33"/>
  </svg>)";

/// One text element drawn three times, itself and twice through `<use>`. A repeat may not rewrite
/// the records that an earlier drawing's batch in the same frame reads, so each repeat borrows
/// records for the frame.
constexpr std::string_view kRepeatedTextSvg = R"(
  <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" width="200" height="200"
       font-family="Noto Sans" font-size="24">
    <text id="words" x="10" y="40" fill="black">repeated words</text>
    <use href="#words" y="50"/>
    <use href="#words" y="100"/>
  </svg>)";

/// Hermetic test fonts, relative to the runfiles root, so glyph identity does not depend on the
/// host's installed fonts.
constexpr std::string_view kFontsRunfilesPath = "third_party/resvg-test-suite/fonts";

/// Parses \p svg, a document without text. @param svg Document source.
SVGDocument ParseShapes(std::string_view svg = kShapesSvg) {
  ParseWarningSink sink = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(svg, sink);
  EXPECT_FALSE(parsed.hasError()) << parsed.error().reason;
  return std::move(parsed.result());
}

/// Parses \p svg with the hermetic test fonts registered. @param svg Document source.
SVGDocument ParseText(std::string_view svg = kTextSvg) {
  ParseWarningSink sink = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(svg, sink);
  EXPECT_FALSE(parsed.hasError()) << parsed.error().reason;
  SVGDocument document = std::move(parsed.result());
  TrustDocumentFontFacesForTesting(document);
  RegisterFontsFromDirectoryForTesting(
      document, Runfiles::instance().Rlocation(std::string(kFontsRunfilesPath)));
  return document;
}

/// The GPU work a frame spends establishing residence: what a frame that finds its geometry,
/// records and bind groups already resident does not repeat.
struct ResidencyWork {
  uint64_t bufferCreates = 0;     //!< Buffers created, slab chunks among them.
  uint64_t bindgroupCreates = 0;  //!< Bind groups created.
  uint64_t bufferWrites = 0;      //!< Buffer uploads.
  uint64_t bufferWriteBytes = 0;  //!< Bytes those uploads carried.

  /// Equality operator. @param other Work to compare against.
  bool operator==(const ResidencyWork& other) const = default;
};

/// Prints \ref ResidencyWork field by field. @param value Work. @param os Output stream.
void PrintTo(const ResidencyWork& value, std::ostream* os) {
  *os << "{bufferCreates=" << value.bufferCreates << " bindgroupCreates=" << value.bindgroupCreates
      << " bufferWrites=" << value.bufferWrites << " bufferWriteBytes=" << value.bufferWriteBytes
      << "}";
}

/// Draws \p document once on \p renderer and returns the residence work that frame did.
ResidencyWork DrawAndMeasure(RendererGeode& renderer, SVGDocument& document) {
  renderer.draw(document);
  const geode::GeodeCounters counters = renderer.lastFrameTimings().counters;
  return ResidencyWork{counters.bufferCreates, counters.bindgroupCreates, counters.bufferWrites,
                       counters.bufferWriteBytes};
}

/// The viewport every document here is drawn at: the documents' own 200 by 200 pixels.
RenderViewport DocumentViewport() {
  RenderViewport viewport;
  viewport.size = Vector2d(200, 200);
  return viewport;
}

/// Draws \p document through the driver's interruptible entry point, as an editor's render worker
/// does, rather than through `RendererGeode::draw`.
///
/// @param driver Driver over the renderer to draw with.
/// @param document Document to draw, whole.
/// @return Whether the frame completed.
bool DrawLikeARenderWorker(RendererDriver& driver, SVGDocument& document) {
  return driver.drawInterruptibly(document, DocumentViewport(), Transform2d(),
                                  [] { return false; });
}

/// Admit one glyph outline to \p renderer's glyph cache, so each frame makes every other glyph
/// resident for that frame only. @param renderer Renderer to limit.
void AdmitOneCachedGlyph(RendererGeode& renderer) {
  renderer.setGlyphResidencyBudgetForTesting(1u, std::numeric_limits<uint64_t>::max());
}

/// The geometry budget of \p document, which every device's residence for it is charged to, or
/// null before any renderer has drawn it.
std::shared_ptr<geode::GeodeDocumentGeometryBudget> DocumentBudgetOf(SVGDocument& document) {
  auto* budget =
      document.registry().ctx().find<std::shared_ptr<geode::GeodeDocumentGeometryBudget>>();
  return budget != nullptr ? *budget : nullptr;
}

/// Cache plus resident geometry bytes charged to \p document's budget, or 0 before any renderer has
/// drawn it. @param document Document to measure.
uint64_t RetainedBytesOf(SVGDocument& document) {
  const std::shared_ptr<geode::GeodeDocumentGeometryBudget> budget = DocumentBudgetOf(document);
  return budget ? budget->cacheBytes() + budget->residentBytes() : 0;
}

/// Draws \p document into one frame of \p renderer, holding the document for the whole frame as a
/// multi-document frame does, and returns what the frame reports.
///
/// @param renderer Renderer to draw with.
/// @param document Document to draw, whole.
/// @param offscreenPassAfter Whether an offscreen pass, such as a filter's, opens and ends inside
/// the
///   frame after the document is drawn.
/// @return The frame's resource stats, read after it ended.
RendererResourceStats DrawOneFrame(RendererGeode& renderer, SVGDocument& document,
                                   bool offscreenPassAfter) {
  const RenderViewport viewport = DocumentViewport();
  const DocumentWriteAccess access = document.writeAccess();
  renderer.beginFrame(viewport);
  RendererDriver driver(renderer);
  driver.drawDocumentIntoCurrentFrame(document, viewport, Transform2d());
  if (offscreenPassAfter) {
    const std::unique_ptr<RendererInterface> offscreen = renderer.createOffscreenInstance();
    EXPECT_THAT(offscreen, NotNull());
    if (offscreen) {
      offscreen->beginFrame(viewport);
      offscreen->endFrame();
    }
  }
  renderer.endFrame();
  return renderer.resourceStats();
}

/// A second logical context over the physical root \p root holds, the way an editor's UI context
/// shares the root its render worker draws through.
std::shared_ptr<geode::GeodeDevice> LogicalContextOver(const geode::GeodeDevice& root) {
  geode::GeodeEmbedConfig config;
  config.physicalDevice = root.physicalDeviceOwner();
  config.textureFormat = geode::WgpuTextureFormatFrom(root.textureFormat());
  return geode::GeodeDevice::CreateFromExternal(config);
}

/**
 * Draws \p document on \p worker from a second thread and on \p ui from this one, taking turns so
 * every worker frame lands between two UI frames, as a render worker's frames land between a UI
 * thread's thumbnail refreshes. The UI thread does not wait for the worker's frame to start: it
 * keeps using its device meanwhile, uploading and dropping geometry of its own.
 *
 * @param worker Device the second thread draws on.
 * @param ui Device this thread draws on.
 * @param document Document both draw.
 */
void DrawOnTwoThreads(const std::shared_ptr<geode::GeodeDevice>& worker,
                      const std::shared_ptr<geode::GeodeDevice>& ui, SVGDocument& document) {
  // Shared between threads the way the editor shares its document: every draw below takes the
  // document's write access, so the two threads only ever touch the document one at a time.
  document.setThreadingMode(ThreadingMode::ConcurrentDom);
  constexpr int kFrames = 8;

  std::mutex turnMutex;
  std::condition_variable turnChanged;
  int workerFramesAllowed = 0;
  int workerFramesDone = 0;
  std::atomic<int> workerIncompleteFrames = 0;

  std::thread workerThread([&] {
    RendererGeode onWorker(worker);
    RendererDriver driver(onWorker);
    for (int frame = 0; frame < kFrames; ++frame) {
      {
        std::unique_lock<std::mutex> lock(turnMutex);
        turnChanged.wait(lock, [&] { return workerFramesAllowed > frame; });
      }
      if (!DrawLikeARenderWorker(driver, document)) {
        ++workerIncompleteFrames;
      }
      {
        std::lock_guard<std::mutex> lock(turnMutex);
        ++workerFramesDone;
      }
      turnChanged.notify_all();
    }
  });

  RendererGeode onUi(ui);
  RendererDriver uiDriver(onUi);
  gpu::Device& uiRuntime = ui->runtimeDevice();
  int uiIncompleteFrames = 0;
  for (int frame = 0; frame < kFrames; ++frame) {
    // A thumbnail: drawn, then read, so the frame's work has finished on the GPU.
    if (!DrawLikeARenderWorker(uiDriver, document)) {
      ++uiIncompleteFrames;
    }
    EXPECT_THAT(onUi.takeSnapshot().empty(), IsFalse());
    {
      std::lock_guard<std::mutex> lock(turnMutex);
      ++workerFramesAllowed;
    }
    turnChanged.notify_all();

    for (int upload = 0; upload < 8; ++upload) {
      gpu::Result<gpu::Buffer> buffer = uiRuntime.createBuffer(gpu::BufferDescriptor{
          "uiGeometry", 256, gpu::BufferUsage::Vertex | gpu::BufferUsage::CopyDst});
      EXPECT_THAT(buffer, gpu::HasResult());
    }

    std::unique_lock<std::mutex> lock(turnMutex);
    turnChanged.wait(lock, [&] { return workerFramesDone > frame; });
  }
  workerThread.join();

  EXPECT_THAT(uiIncompleteFrames, Eq(0));
  EXPECT_THAT(workerIncompleteFrames.load(), Eq(0));
  EXPECT_THAT(onUi.deviceLost(), IsFalse());
}

/// Two contexts over two separate roots.
class GeodeDocumentResidencyTest : public testing::Test {
protected:
  void SetUp() override {
    first_ = geode::GeodeDevice::CreateHeadless();
    second_ = geode::GeodeDevice::CreateHeadless();
    ASSERT_THAT(first_, NotNull());
    ASSERT_THAT(second_, NotNull());
  }

  std::shared_ptr<geode::GeodeDevice> first_;
  std::shared_ptr<geode::GeodeDevice> second_;
};

/// The editor's shape: the context created with a root, which renders through the root's own
/// device and retires into the root owner's retirement, and a logical context over the same root
/// with a runtime device and a retirement of its own.
class GeodeSharedRootResidencyTest : public testing::Test {
protected:
  void SetUp() override {
    root_ = geode::GeodeDevice::CreateHeadless();
    ASSERT_THAT(root_, NotNull());
    logical_ = LogicalContextOver(*root_);
    ASSERT_THAT(logical_, NotNull());
  }

  std::shared_ptr<geode::GeodeDevice> root_;
  std::shared_ptr<geode::GeodeDevice> logical_;
};

TEST_F(GeodeDocumentResidencyTest, ADeviceKeepsItsResidenceWhenAnotherDeviceDrawsTheDocument) {
  SVGDocument document = ParseShapes();
  const int64_t firstBaseline = first_->liveResidentBytesForTesting();
  RendererGeode onFirst(first_);
  onFirst.draw(document);
  const int64_t firstResident = first_->liveResidentBytesForTesting();
  ASSERT_THAT(firstResident, Gt(firstBaseline)) << "the document must establish residence";

  RendererGeode onSecond(second_);
  onSecond.draw(document);

  EXPECT_THAT(first_->liveResidentBytesForTesting(), Eq(firstResident))
      << "drawing the document on another device must not release this device's residence";
  EXPECT_THAT(second_->liveResidentBytesForTesting(), Gt(int64_t{0}));
}

TEST_F(GeodeDocumentResidencyTest, AnotherDeviceDrawingTheDocumentCostsThisDeviceNothing) {
  SVGDocument document = ParseShapes();
  RendererGeode onFirst(first_);
  (void)DrawAndMeasure(onFirst, document);
  (void)DrawAndMeasure(onFirst, document);
  const ResidencyWork steady = DrawAndMeasure(onFirst, document);

  // Another device draws the same document in between, as a thumbnail pass on a UI device does
  // between two frames of a render worker.
  RendererGeode onSecond(second_);
  onSecond.draw(document);

  EXPECT_THAT(DrawAndMeasure(onFirst, document), Eq(steady))
      << "the next frame on this device must find its residence where it left it, not rebuild it";
}

TEST_F(GeodeDocumentResidencyTest, TwoDevicesOnTwoThreadsShareOneDocument) {
  SVGDocument document = ParseShapes();
  DrawOnTwoThreads(/*worker=*/second_, /*ui=*/first_, document);
}

TEST_F(GeodeDocumentResidencyTest, ADocumentDestroyedOnAnotherThreadHandsEachDeviceItsHandles) {
  std::optional<SVGDocument> document = ParseShapes();
  RendererGeode onFirst(first_);
  onFirst.draw(*document);
  RendererGeode onSecond(second_);
  onSecond.draw(*document);
  for (const std::shared_ptr<geode::GeodeDevice>& device : {first_, second_}) {
    ASSERT_THAT(device->retiredHandleCountsForTesting().buffers, Eq(0u));
    ASSERT_THAT(device->retiredHandleCountsForTesting().bindGroups, Eq(0u));
  }

  // The document goes on a thread that is neither device's.
  std::thread([&] { document.reset(); }).join();

  // Both kinds come back: the slabs' chunk buffers and the bind groups the slots had cached.
  for (const std::shared_ptr<geode::GeodeDevice>& device : {first_, second_}) {
    const geode::GeodeHandleRetirement::HeldCounts held = device->retiredHandleCountsForTesting();
    EXPECT_THAT(held.buffers, Gt(0u))
        << "the document's buffers on this device must come back to it, not be released on the "
           "thread that destroyed the document";
    EXPECT_THAT(held.bindGroups, Gt(0u)) << "so must its bind groups";
  }

  // Each device releases what came back at its next frame boundary, on its own thread.
  SVGDocument next = ParseShapes();
  onFirst.draw(next);
  onSecond.draw(next);
  for (const std::shared_ptr<geode::GeodeDevice>& device : {first_, second_}) {
    EXPECT_THAT(device->retiredHandleCountsForTesting().buffers, Eq(0u));
    EXPECT_THAT(device->retiredHandleCountsForTesting().bindGroups, Eq(0u));
  }
}

TEST_F(GeodeDocumentResidencyTest, ADocumentOutlivesADeviceThatDrewIt) {
  SVGDocument document = ParseShapes();
  {
    RendererGeode onFirst(first_);
    onFirst.draw(document);
  }
  // The device goes while the document still holds residence built on it.
  first_.reset();

  RendererGeode onSecond(second_);
  const ResidencyWork firstFrame = DrawAndMeasure(onSecond, document);
  EXPECT_THAT(firstFrame.bufferWrites, Gt(0u)) << "the surviving device builds its own residence";
  EXPECT_THAT(onSecond.deviceLost(), IsFalse());
  EXPECT_THAT(second_->liveResidentBytesForTesting(), Gt(int64_t{0}));
}

TEST_F(GeodeDocumentResidencyTest, TextKeepsItsResidenceWhenAnotherDeviceDrawsIt) {
  SVGDocument document = ParseText();
  RendererGeode onFirst(first_);
  (void)DrawAndMeasure(onFirst, document);
  (void)DrawAndMeasure(onFirst, document);
  const ResidencyWork steady = DrawAndMeasure(onFirst, document);
  const size_t firstGlyphs = onFirst.residentGlyphCountForTesting(document);
  ASSERT_THAT(firstGlyphs, Gt(0u)) << "the text must make its glyph outlines resident";

  RendererGeode onSecond(second_);
  onSecond.draw(document);
  EXPECT_THAT(onSecond.residentGlyphCountForTesting(document), Eq(firstGlyphs))
      << "the second device keeps glyph outlines of its own";

  EXPECT_THAT(onFirst.residentGlyphCountForTesting(document), Eq(firstGlyphs));
  EXPECT_THAT(DrawAndMeasure(onFirst, document), Eq(steady))
      << "the glyph cache and the text records on this device must survive another device "
         "drawing the text";
}

TEST_F(GeodeDocumentResidencyTest, AnEditOnOneDeviceReachesTheOtherDevicesResidence) {
  SVGDocument document = ParseShapes();
  RendererGeode onFirst(first_);
  RendererGeode onSecond(second_);
  onFirst.draw(document);
  onSecond.draw(document);

  std::optional<SVGElement> curve = document.querySelector("#curve");
  ASSERT_TRUE(curve.has_value());
  curve->cast<SVGPathElement>().setD(RcString("M20 20 L180 180"));

  // The first device draws the edit first; the second must not keep drawing its old upload.
  onFirst.draw(document);
  const RendererBitmap onFirstAfterEdit = onFirst.takeSnapshot();
  onSecond.draw(document);
  const RendererBitmap onSecondAfterEdit = onSecond.takeSnapshot();

  // Both devices must draw the edited path, pixel for pixel.
  ASSERT_FALSE(onFirstAfterEdit.empty());
  editor::tests::CompareBitmapToBitmap(onSecondAfterEdit, onFirstAfterEdit,
                                       "edit_reaches_the_other_devices_residence",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(GeodeDocumentResidencyTest, ADeviceThatGoesStopsCountingAgainstTheDocumentBudget) {
  SVGDocument document = ParseShapes();
  std::optional<SVGElement> box = document.querySelector("#box");
  ASSERT_TRUE(box.has_value());
  constexpr Vector2i kThumbnailSizePx(64, 64);

  // The first device draws only one element, as a thumbnail pass does.
  Renderer thumbnails(first_);
  ASSERT_THAT(thumbnails.renderElement(*box, kThumbnailSizePx).empty(), IsFalse());
  const std::shared_ptr<geode::GeodeDocumentGeometryBudget> budget = DocumentBudgetOf(document);
  ASSERT_THAT(budget, NotNull());
  const uint64_t firstShare = budget->residentBytes();
  ASSERT_THAT(firstShare, Gt(0u));

  // The second device draws all of it, and then goes.
  {
    RendererGeode onSecond(second_);
    onSecond.draw(document);
  }
  ASSERT_THAT(budget->residentBytes(), Gt(firstShare));
  second_.reset();

  // The next draw finds the second device gone. Elements the first device never draws still held
  // the second device's slots; they must not keep its residence charged to the document.
  ASSERT_THAT(thumbnails.renderElement(*box, kThumbnailSizePx).empty(), IsFalse());
  EXPECT_THAT(budget->residentBytes(), Eq(firstShare));
}

TEST_F(GeodeDocumentResidencyTest, ADocumentBudgetRefusalRecoversWhenTheDeviceHoldingItGoes) {
  SVGDocument document = ParseShapes();
  std::optional<RendererGeode> onFirst(std::in_place, first_);
  onFirst->draw(document);
  const std::shared_ptr<geode::GeodeDocumentGeometryBudget> budget = DocumentBudgetOf(document);
  ASSERT_THAT(budget, NotNull());
  const uint64_t firstShare = budget->residentBytes();
  ASSERT_THAT(firstShare, Gt(0u));
  // Room for exactly one device's residence.
  budget->setLimitsForTesting({.cacheBytes = geode::GeodeDocumentGeometryBudget::kMaximumCacheBytes,
                               .residentBytes = firstShare});

  // A second device finds the budget full and draws without residence.
  {
    RendererGeode onSecond(second_);
    onSecond.draw(document);
    EXPECT_THAT(onSecond.deviceLost(), IsFalse());
  }
  ASSERT_THAT(second_->liveResidentBytesForTesting(), Eq(int64_t{0}));
  ASSERT_THAT(budget->rejected(), IsTrue());

  // The device holding the budget goes, and a third device draws the document.
  onFirst.reset();
  first_.reset();
  const std::shared_ptr<geode::GeodeDevice> third = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(third, NotNull());
  RendererGeode onThird(third);
  onThird.draw(document);

  EXPECT_THAT(third->liveResidentBytesForTesting(), Gt(int64_t{0}))
      << "an earlier refusal must not refuse residence once the budget has room again";
}

TEST_F(GeodeDocumentResidencyTest, RecordsARepeatBorrowedGoWithTheDocumentOnceItsFrameEnds) {
  if (!RendererGeode::sceneBatchingEnabledForTesting()) {
    GTEST_SKIP() << "Without scene batching a repeat draws solo and borrows no records.";
  }
  std::optional<SVGDocument> document = ParseText(kRepeatedTextSvg);
  RendererGeode renderer(first_);
  renderer.draw(*document);
  const std::shared_ptr<geode::GeodeDocumentGeometryBudget> budget = DocumentBudgetOf(*document);
  ASSERT_THAT(budget, NotNull());
  ASSERT_THAT(budget->residentBytes(), Gt(0u));

  // The renderer draws nothing more. Had it kept the records its repeats borrowed until its next
  // frame, they would keep the document's record slab, and its charge, after the document went.
  document.reset();
  EXPECT_THAT(budget->residentBytes(), Eq(0u))
      << "the records the repeats borrowed must go back when the frame ends";
}

TEST_F(GeodeDocumentResidencyTest, GlyphsPastTheCacheGoWithTheDocumentOnceItsFrameEnds) {
  std::optional<SVGDocument> document = ParseText();
  RendererGeode renderer(first_);
  AdmitOneCachedGlyph(renderer);
  renderer.draw(*document);
  ASSERT_THAT(renderer.residentGlyphCountForTesting(*document), Eq(1u))
      << "the text's other glyphs must be resident for the frame only";
  const std::shared_ptr<geode::GeodeDocumentGeometryBudget> budget = DocumentBudgetOf(*document);
  ASSERT_THAT(budget, NotNull());

  document.reset();
  EXPECT_THAT(budget->residentBytes(), Eq(0u))
      << "a glyph resident for one frame must give its geometry back when the frame ends";
  EXPECT_THAT(budget->cacheBytes(), Eq(0u)) << "and the bytes its encode was charged";
}

TEST_F(GeodeDocumentResidencyTest, ARendererKeepsNoDocumentBudgetOnceItsFrameEnds) {
  std::optional<SVGDocument> document = ParseShapes();
  RendererGeode renderer(first_);
  renderer.draw(*document);
  const std::weak_ptr<geode::GeodeDocumentGeometryBudget> budget = DocumentBudgetOf(*document);
  ASSERT_THAT(budget.expired(), IsFalse());
  const RendererResourceStats atFrameEnd = renderer.resourceStats();
  ASSERT_THAT(atFrameEnd.geometryRetainedBytes, Gt(0u));

  document.reset();
  EXPECT_THAT(budget.expired(), IsTrue())
      << "the renderer must not hold the budget of a document whose frame has ended";
  EXPECT_THAT(renderer.resourceStats().geometryRetainedBytes, Eq(atFrameEnd.geometryRetainedBytes))
      << "a frame's figures describe the frame, whatever becomes of its documents afterwards";
}

TEST_F(GeodeDocumentResidencyTest, ARenderWorkerReusesTheRecordsItsRepeatsBorrowed) {
  if (!RendererGeode::sceneBatchingEnabledForTesting()) {
    GTEST_SKIP() << "Without scene batching a repeat draws solo and borrows no records.";
  }
  SVGDocument document = ParseText(kRepeatedTextSvg);
  RendererGeode renderer(first_);
  RendererDriver driver(renderer);
  ASSERT_THAT(DrawLikeARenderWorker(driver, document), IsTrue());
  ASSERT_THAT(DrawLikeARenderWorker(driver, document), IsTrue());
  const std::shared_ptr<geode::GeodeDocumentGeometryBudget> budget = DocumentBudgetOf(document);
  ASSERT_THAT(budget, NotNull());
  const uint64_t steadyResidentBytes = budget->residentBytes();

  // The repeats borrow a record for each glyph they draw, every frame. Over this many frames that
  // is more records than the first record buffer holds.
  constexpr int kFrames = 64;
  for (int frame = 0; frame < kFrames; ++frame) {
    ASSERT_THAT(DrawLikeARenderWorker(driver, document), IsTrue());
  }
  EXPECT_THAT(budget->residentBytes(), Eq(steadyResidentBytes))
      << "each frame must reuse the records earlier frames borrowed, not keep borrowing more";
}

TEST_F(GeodeDocumentResidencyTest, ADocumentDestroyedWhileItsRendererDrawsTheNextIsLeftAlone) {
  std::optional<SVGDocument> document = ParseText(kRepeatedTextSvg);
  document->setThreadingMode(ThreadingMode::ConcurrentDom);
  RendererGeode renderer(first_);
  // Both kinds of per-frame loan: records for the repeats and glyphs past the cache.
  AdmitOneCachedGlyph(renderer);

  std::atomic<bool> drawn = false;
  std::thread worker([&] {
    RendererDriver driver(renderer);
    EXPECT_THAT(DrawLikeARenderWorker(driver, *document), IsTrue());
    drawn.store(true, std::memory_order_release);
    // The next document, drawn the ordinary way. Nothing of the first may be touched from here.
    SVGDocument next = ParseShapes();
    renderer.draw(next);
  });
  while (!drawn.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  // The editor's shape: the UI thread replaces the document while the render worker moves on.
  // Nothing orders the two, and nothing needs to.
  document.reset();
  worker.join();
  EXPECT_THAT(renderer.deviceLost(), IsFalse());
}

TEST_F(GeodeDocumentResidencyTest, AFramesResourceStatsNeedNoDocumentAccess) {
  SVGDocument document = ParseShapes();
  document.setThreadingMode(ThreadingMode::ConcurrentDom);
  RendererGeode onFirst(first_);
  RendererDriver firstDriver(onFirst);
  ASSERT_THAT(DrawLikeARenderWorker(firstDriver, document), IsTrue());

  // Another device draws the document, charging its residence to the document's budget, while
  // this renderer reports on its finished frame without holding the document.
  std::thread other([&] {
    RendererGeode onSecond(second_);
    RendererDriver secondDriver(onSecond);
    EXPECT_THAT(DrawLikeARenderWorker(secondDriver, document), IsTrue());
  });
  const RendererResourceStats stats = onFirst.resourceStats();
  other.join();
  EXPECT_THAT(stats.geometryRetainedBytes, Gt(0u));
}

TEST_F(GeodeDocumentResidencyTest, AnOffscreenPassEndingFirstLeavesTheFramesDocumentToTheFrame) {
  // Two copies of one document: a frame without the pass shows what a frame with it must report.
  SVGDocument alone = ParseShapes(kLoneRectSvg);
  SVGDocument withPass = ParseShapes(kLoneRectSvg);
  RendererGeode renderer(first_);

  const RendererResourceStats expected = DrawOneFrame(renderer, alone, false);
  const RendererResourceStats actual = DrawOneFrame(renderer, withPass, true);
  ASSERT_THAT(RetainedBytesOf(withPass), Eq(RetainedBytesOf(alone)))
      << "both copies must end their frames retaining the same bytes";

  // The pass ended while the frame's rectangle was still pending. The frame gave it residence at
  // its end, and must report its document as it left it, not as the pass saw it.
  EXPECT_THAT(actual.geometryRetainedBytes, Eq(expected.geometryRetainedBytes));
}

TEST_F(GeodeDocumentResidencyTest, AnOffscreenPassLetsGoOfTheDocumentItDrewWhenItEnds) {
  std::optional<SVGDocument> nested = ParseShapes();
  RendererGeode renderer(first_);
  const RenderViewport viewport = DocumentViewport();
  renderer.beginFrame(viewport);

  // An offscreen pass inside the frame draws another document while that document is held, as a
  // filter's feImage draws a nested SVG.
  const std::unique_ptr<RendererInterface> offscreen = renderer.createOffscreenInstance();
  ASSERT_THAT(offscreen, NotNull());
  std::weak_ptr<geode::GeodeDocumentGeometryBudget> nestedBudget;
  uint64_t nestedBytes = 0;
  {
    const DocumentWriteAccess access = nested->writeAccess();
    offscreen->beginFrame(viewport);
    RendererDriver driver(*offscreen);
    driver.drawDocumentIntoCurrentFrame(*nested, viewport, Transform2d());
    offscreen->endFrame();
    nestedBudget = DocumentBudgetOf(*nested);
    nestedBytes = RetainedBytesOf(*nested);
  }
  ASSERT_THAT(nestedBytes, Gt(0u));

  // The document goes while the outer frame is still open. The pass that drew it has ended, so
  // nothing may still hold its budget, the outer frame included.
  nested.reset();
  EXPECT_THAT(nestedBudget.expired(), IsTrue())
      << "the pass must settle the document it drew when it ends, not leave it to the outer frame";

  renderer.endFrame();
  EXPECT_THAT(renderer.resourceStats().geometryRetainedBytes, Ge(nestedBytes))
      << "the frame still reports what its offscreen pass drew";
}

TEST_F(GeodeSharedRootResidencyTest, EachRetirementOutlivesTheDeviceItReleasesInto) {
  // A retirement destroyed before its device would let a document destroyed on another thread
  // meanwhile drop that device's handles in place, into tables the device's own thread is using.
  EXPECT_THAT(logical_->retirementOutlivesOwnedRuntimeDeviceForTesting(), IsTrue());
  EXPECT_THAT(root_->physicalDeviceOwner()->rootRetirementOutlivesRootDeviceForTesting(), IsTrue());
}

TEST_F(GeodeSharedRootResidencyTest, TheEditorsTwoContextsShareOneDocumentOnTwoThreads) {
  SVGDocument document = ParseShapes();
  // The render worker draws through the root context, the UI through the logical one.
  DrawOnTwoThreads(/*worker=*/root_, /*ui=*/logical_, document);
}

TEST_F(GeodeSharedRootResidencyTest, TheEditorsTwoContextsShareOneTextDocumentOnTwoThreads) {
  SVGDocument document = ParseText();
  DrawOnTwoThreads(/*worker=*/root_, /*ui=*/logical_, document);
}

TEST_F(GeodeSharedRootResidencyTest, AContextGoesWhileAnotherThreadDestroysADocumentItDrew) {
  std::optional<SVGDocument> document = ParseShapes();
  {
    RendererGeode onRoot(root_);
    onRoot.draw(*document);
    RendererGeode onLogical(logical_);
    onLogical.draw(*document);
  }

  // One thread lets the logical context go while another destroys the document holding its
  // residence: the document's slabs retire into the logical context's retirement as it closes.
  std::atomic<int> started = 0;
  const auto startTogether = [&started] {
    started.fetch_add(1, std::memory_order_acq_rel);
    while (started.load(std::memory_order_acquire) < 2) {
      std::this_thread::yield();
    }
  };
  std::thread contextGoes([&] {
    startTogether();
    logical_.reset();
  });
  std::thread documentGoes([&] {
    startTogether();
    document.reset();
  });
  contextGoes.join();
  documentGoes.join();

  // The root context, which drew the same document, is unaffected.
  RendererGeode onRoot(root_);
  SVGDocument next = ParseShapes();
  onRoot.draw(next);
  EXPECT_THAT(onRoot.deviceLost(), IsFalse());
  EXPECT_THAT(onRoot.takeSnapshot().empty(), IsFalse());
}

TEST_F(GeodeSharedRootResidencyTest, TheRootContextGoingLeavesALogicalContextDrawing) {
  SVGDocument document = ParseShapes();
  const std::shared_ptr<geode::GeodePhysicalDeviceOwner> owner = root_->physicalDeviceOwner();
  {
    RendererGeode onRoot(root_);
    onRoot.draw(document);
  }
  RendererGeode onLogical(logical_);
  (void)DrawAndMeasure(onLogical, document);
  (void)DrawAndMeasure(onLogical, document);
  const ResidencyWork steady = DrawAndMeasure(onLogical, document);

  // The root context goes; the root device stays, held by the owner the logical context keeps.
  root_.reset();
  EXPECT_THAT(DrawAndMeasure(onLogical, document), Eq(steady))
      << "the logical context's residence is its own";
  EXPECT_THAT(onLogical.deviceLost(), IsFalse());

  // That draw dropped the gone context's residence. Its handles wait in the retirement the owner
  // holds, and go only with the owner, after the root device.
  EXPECT_THAT(owner->rootDeviceHandleRetirement()->closed(), IsTrue());
  EXPECT_THAT(owner->rootDeviceHandleRetirement()->heldCountsForTesting().buffers, Gt(0u));
}

}  // namespace
}  // namespace donner::svg
