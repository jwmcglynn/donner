/// @file
/// One document drawn by renderers on two devices. Each device keeps its own GPU residence for the
/// document, so drawing on one device neither releases nor rebuilds the other's, nothing one device
/// owns is released on another device's thread, and a document may outlive a device that drew it.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <thread>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeCounters.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"

namespace donner::svg {
namespace {

using testing::Eq;
using testing::Gt;
using testing::IsFalse;
using testing::NotNull;

constexpr std::string_view kShapesSvg = R"(
  <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" width="200" height="200">
    <rect x="10" y="10" width="80" height="60" fill="#c33"/>
    <circle cx="140" cy="60" r="40" fill="#3a6" stroke="#036" stroke-width="6"/>
    <path d="M20 180 C60 110 120 210 180 130" fill="none" stroke="#639" stroke-width="8"/>
    <ellipse cx="100" cy="150" rx="50" ry="20" fill="#fc3" opacity="0.8"/>
  </svg>)";

SVGDocument ParseShapes() {
  ParseWarningSink sink = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(kShapesSvg, sink);
  EXPECT_FALSE(parsed.hasError()) << parsed.error().reason;
  return std::move(parsed.result());
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
  // Shared between threads the way the editor shares its document: every draw below takes the
  // document's write access, so the two threads only ever touch the document one at a time.
  document.setThreadingMode(ThreadingMode::ConcurrentDom);
  constexpr int kFrames = 8;
  RenderViewport viewport;
  viewport.size = Vector2d(200, 200);
  const auto neverCancel = [] { return false; };

  // The threads take turns drawing, so every worker frame lands between two UI frames, as a render
  // worker's frames land between a UI thread's thumbnail refreshes. The UI thread does not wait
  // for the worker's frame to start: it keeps using its device meanwhile.
  std::mutex turnMutex;
  std::condition_variable turnChanged;
  int workerFramesAllowed = 0;
  int workerFramesDone = 0;
  std::atomic<int> workerIncompleteFrames = 0;

  std::thread worker([&] {
    RendererGeode onWorker(second_);
    RendererDriver driver(onWorker);
    for (int frame = 0; frame < kFrames; ++frame) {
      {
        std::unique_lock<std::mutex> lock(turnMutex);
        turnChanged.wait(lock, [&] { return workerFramesAllowed > frame; });
      }
      if (!driver.drawInterruptibly(document, viewport, Transform2d(), neverCancel)) {
        ++workerIncompleteFrames;
      }
      {
        std::lock_guard<std::mutex> lock(turnMutex);
        ++workerFramesDone;
      }
      turnChanged.notify_all();
    }
  });

  RendererGeode onUi(first_);
  RendererDriver uiDriver(onUi);
  gpu::Device& uiRuntime = first_->runtimeDevice();
  int uiIncompleteFrames = 0;
  for (int frame = 0; frame < kFrames; ++frame) {
    // A thumbnail: drawn, then read, so the frame's work has finished on the GPU.
    if (!uiDriver.drawInterruptibly(document, viewport, Transform2d(), neverCancel)) {
      ++uiIncompleteFrames;
    }
    EXPECT_THAT(onUi.takeSnapshot().empty(), IsFalse());
    {
      std::lock_guard<std::mutex> lock(turnMutex);
      ++workerFramesAllowed;
    }
    turnChanged.notify_all();

    // While the worker draws, the UI thread uploads and drops geometry of its own on its device,
    // as a UI renderer does every frame.
    for (int upload = 0; upload < 8; ++upload) {
      gpu::Result<gpu::Buffer> buffer = uiRuntime.createBuffer(gpu::BufferDescriptor{
          "uiGeometry", 256, gpu::BufferUsage::Vertex | gpu::BufferUsage::CopyDst});
      EXPECT_THAT(buffer, gpu::HasResult());
    }

    std::unique_lock<std::mutex> lock(turnMutex);
    turnChanged.wait(lock, [&] { return workerFramesDone > frame; });
  }
  worker.join();

  EXPECT_THAT(uiIncompleteFrames, Eq(0));
  EXPECT_THAT(workerIncompleteFrames.load(), Eq(0));
  EXPECT_THAT(onUi.deviceLost(), IsFalse());
}

TEST_F(GeodeDocumentResidencyTest, ADocumentDestroyedOnAnotherThreadHandsEachDeviceItsHandles) {
  std::optional<SVGDocument> document = ParseShapes();
  RendererGeode onFirst(first_);
  onFirst.draw(*document);
  RendererGeode onSecond(second_);
  onSecond.draw(*document);
  ASSERT_THAT(first_->retiredHandleCountForTesting(), Eq(0u));
  ASSERT_THAT(second_->retiredHandleCountForTesting(), Eq(0u));

  // The document goes on a thread that is neither device's.
  std::thread([&] { document.reset(); }).join();

  EXPECT_THAT(first_->retiredHandleCountForTesting(), Gt(0u))
      << "the document's residence on this device must come back to it, not be released on the "
         "thread that destroyed the document";
  EXPECT_THAT(second_->retiredHandleCountForTesting(), Gt(0u));

  // Each device releases what came back at its next frame boundary, on its own thread.
  SVGDocument next = ParseShapes();
  onFirst.draw(next);
  onSecond.draw(next);
  EXPECT_THAT(first_->retiredHandleCountForTesting(), Eq(0u));
  EXPECT_THAT(second_->retiredHandleCountForTesting(), Eq(0u));
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

}  // namespace
}  // namespace donner::svg
