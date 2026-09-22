/// @file
/// Cross-renderer frame-generation claims on one shared `GeodeDevice`.
///
/// Two renderers routinely share a device: an offscreen instance renders an
/// feImage fragment or a layer thumbnail of the SAME document while the outer
/// renderer's frame is still open and has already recorded draws. Frame
/// generations are device-scoped and monotonic, so the inner frame's index
/// never equals the outer's; any "was this touched in the current frame?"
/// equality gate therefore reads the outer frame's resident slots as free.
/// Rewriting a slot's instance record or gradient paint block then corrupts
/// the outer frame retroactively, because every queue write in a frame lands
/// before every draw in that frame's submit.
///
/// Contract under test:
///  1. `GeodeDevice::frameStampClaimed` claims exactly the stamps at or after
///     the oldest still-open generation, and never the never-drawn sentinel.
///  2. End to end: a mid-frame offscreen render of the same document must not
///     repaint pixels an outer frame has already recorded (the outer frame
///     keeps the gradient bytes that were live when its batch was appended).
///  3. Logical contexts over one physical root keep their runtime identity,
///     submission serials and allocation counters to themselves, and share
///     exactly one sticky device-loss condition.
///  4. Contexts over one root can be driven from two threads at once.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "donner/base/ParseWarningSink.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/Device.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeEmbed.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"
#include "donner/svg/renderer/geode/tests/GeodeTestContexts.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"

namespace donner::svg {
namespace {

using test::PixelAt;

/// A flat "gradient" (both stops the same blue) so a paint-block rewrite is a
/// whole-pixel color change rather than a subtle ramp shift. Gradient fill is
/// required: solid fills read their color from the slot uniform, while a
/// gradient reads the slot's paint block, which is exactly the storage an
/// equality-gated inner pass would rewrite.
constexpr std::string_view kGradientRectSvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
  <defs>
    <linearGradient id="g" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="100" y2="0">
      <stop id="s0" offset="0" stop-color="#0000ff"/>
      <stop id="s1" offset="1" stop-color="#0000ff"/>
    </linearGradient>
  </defs>
  <rect id="target" x="10" y="10" width="60" height="60" fill="url(#g)"/>
</svg>
)SVG";

SVGDocument parseDocument(std::string_view svgSource) {
  ParseWarningSink sink = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(svgSource, sink);
  EXPECT_FALSE(parsed.hasError()) << (parsed.hasError() ? parsed.error().reason : "");
  return std::move(parsed.result());
}

bool isBlue(const std::array<uint8_t, 4>& px) {
  return px[2] > 200 && px[0] < 60 && px[1] < 60 && px[3] > 200;
}

class GeodeSharedDeviceFrameTest : public ::testing::Test {
protected:
  static std::shared_ptr<geode::GeodeDevice> sharedDevice() {
    static auto device = [] {
      return std::shared_ptr<geode::GeodeDevice>(geode::GeodeDevice::CreateHeadless());
    }();
    return device;
  }
};

/// A second logical context over the physical root `root` already holds, or null when the root
/// refuses to share.
std::unique_ptr<geode::GeodeDevice> siblingContextOf(const geode::GeodeDevice& root) {
  geode::GeodeEmbedConfig config;
  config.physicalDevice = root.physicalDeviceOwner();
  config.textureFormat = geode::WgpuTextureFormatFrom(root.textureFormat());
  return geode::GeodeDevice::CreateFromExternal(config);
}

/// Submits one empty command buffer through `runtime` and returns the serial it was given, or 0
/// when the runtime refused any step of it.
uint64_t submitEmptyCommandBuffer(gpu::Device& runtime) {
  gpu::Result<std::unique_ptr<gpu::CommandEncoder>> encoder = runtime.createCommandEncoder();
  if (encoder.hasError()) {
    return 0;
  }
  gpu::Result<gpu::CommandBuffer> commands = encoder.result()->finish();
  if (commands.hasError()) {
    return 0;
  }
  gpu::Result<uint64_t> serial = runtime.submit(std::move(commands).result());
  return serial.hasError() ? 0 : serial.result();
}

// ---------------------------------------------------------------------------
// Logical contexts over one physical root
// ---------------------------------------------------------------------------

/// Two logical contexts over one physical root are two runtime devices: a resource of one is not
/// a resource of the other, so their identities, submission serials and allocation counters have
/// to stay apart. Collapsing them would let a handle minted by one pass validation on the other
/// and name whatever now occupies that slot.
TEST_F(GeodeSharedDeviceFrameTest, ContextsOverOneRootKeepTheirRuntimeStateApart) {
  auto root = sharedDevice();
  ASSERT_TRUE(root) << "GeodeDevice::CreateHeadless failed";
  std::unique_ptr<geode::GeodeDevice> sibling = siblingContextOf(*root);
  ASSERT_NE(sibling, nullptr);

  gpu::Device& rootRuntime = root->runtimeDevice();
  gpu::Device& siblingRuntime = sibling->runtimeDevice();
  EXPECT_THAT(siblingRuntime.deviceId(), testing::Ne(rootRuntime.deviceId()));
  EXPECT_THAT(sibling->deviceId(), testing::Ne(root->deviceId()));

  // Serials are per runtime device, so two submissions on one and one on the other must leave the
  // second runtime a submission behind rather than sharing a counter.
  const uint64_t siblingBefore = siblingRuntime.lastSubmittedSerial();
  const uint64_t rootBefore = rootRuntime.lastSubmittedSerial();
  ASSERT_THAT(submitEmptyCommandBuffer(rootRuntime), testing::Gt(0u));
  ASSERT_THAT(submitEmptyCommandBuffer(rootRuntime), testing::Gt(0u));
  ASSERT_THAT(submitEmptyCommandBuffer(siblingRuntime), testing::Gt(0u));
  EXPECT_THAT(rootRuntime.lastSubmittedSerial(), testing::Eq(rootBefore + 2));
  EXPECT_THAT(siblingRuntime.lastSubmittedSerial(), testing::Eq(siblingBefore + 1));

  // Allocation counters follow the context the allocation was made on.
  const uint64_t rootBuffersBefore = root->lifetimeBufferCreates();
  const uint64_t siblingBuffersBefore = sibling->lifetimeBufferCreates();
  gpu::Result<gpu::Buffer> buffer = siblingRuntime.createBuffer(
      gpu::BufferDescriptor{"siblingProbe", 256, gpu::BufferUsage::CopyDst});
  ASSERT_FALSE(buffer.hasError()) << buffer.error().toString();
  EXPECT_THAT(sibling->lifetimeBufferCreates(), testing::Eq(siblingBuffersBefore + 1));
  EXPECT_THAT(root->lifetimeBufferCreates(), testing::Eq(rootBuffersBefore));
}

/// The physical root is one device, so the condition "this device stopped answering" is one
/// condition. A bounded wait on one context's runtime that reaches its deadline has observed the
/// root hang, and every other context over that root renders through the same hung hardware: a
/// context that still reports a healthy device goes on submitting work that can never complete
/// and waiting its own full budget for each of it.
TEST_F(GeodeSharedDeviceFrameTest, ALossOneContextsWaitObservesIsSharedByTheOthers) {
  // A private root: this case declares the physical device lost, which is sticky, so it must not
  // reach the shared device the rest of the file renders through. It holds work through the
  // transitional adapter's test seam, so it selects that backend by name.
  std::unique_ptr<geode::GeodeDevice> root = geode::CreateTransitionalAdapterContext();
  ASSERT_NE(root, nullptr) << "no wgpu adapter is available on this host";
  std::unique_ptr<geode::GeodeDevice> sibling = siblingContextOf(*root);
  ASSERT_NE(sibling, nullptr);
  ASSERT_FALSE(root->isDeviceLost());
  ASSERT_FALSE(sibling->isDeviceLost());

  geode::GeodeWgpuAdapterDevice& rootRuntime = root->adapterDevice();
  const uint64_t submitted = submitEmptyCommandBuffer(rootRuntime);
  ASSERT_THAT(submitted, testing::Gt(0u));
  // Submitted work that stops retiring, on a device whose poll blocks the way a driver waiting on
  // it does: the wait can only end by spending its budget.
  rootRuntime.holdSubmittedWorkForTesting(submitted - 1, std::chrono::milliseconds(1));
  EXPECT_THAT(rootRuntime.waitForSerial(submitted, 0.25), testing::IsFalse());

  EXPECT_TRUE(root->isDeviceLost());
  EXPECT_TRUE(sibling->isDeviceLost())
      << "a loss observed through one context's runtime is a loss of the root both contexts "
         "render through";

  rootRuntime.holdSubmittedWorkForTesting(geode::GeodeWgpuAdapterDevice::kNoCompletedSerialCeiling,
                                          std::chrono::milliseconds(0));
}

/// An editor drives one context from its UI thread and another from its render worker, both over
/// one root. A submission's completion callback runs on whichever thread next drives the root's
/// queue, so one context's completion regularly runs inside the other context's submit, on the
/// other thread. Each thread's submissions must still complete, and the handoff must be ordered.
TEST_F(GeodeSharedDeviceFrameTest, ContextsOverOneRootSubmitFromTwoThreads) {
  std::unique_ptr<geode::GeodeDevice> root = geode::GeodeDevice::CreateHeadless();
  ASSERT_NE(root, nullptr) << "GeodeDevice::CreateHeadless failed";
  std::unique_ptr<geode::GeodeDevice> sibling = siblingContextOf(*root);
  ASSERT_NE(sibling, nullptr);
  constexpr int kSubmissions = 64;
  constexpr double kWaitSeconds = 5.0;

  // Submits on `runtime` and waits for the last submission. Returns what went wrong, or an empty
  // string.
  const auto submitAndWait = [](gpu::Device& runtime) -> std::string {
    int refusedSubmissions = 0;
    uint64_t last = 0;
    for (int i = 0; i < kSubmissions; ++i) {
      const uint64_t serial = submitEmptyCommandBuffer(runtime);
      if (serial == 0) {
        ++refusedSubmissions;
      } else {
        last = serial;
      }
    }
    if (refusedSubmissions != 0) {
      return std::to_string(refusedSubmissions) + " submissions refused";
    }
    const auto waitStart = std::chrono::steady_clock::now();
    if (!runtime.waitForSerial(last, kWaitSeconds)) {
      const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - waitStart);
      return "the wait for serial " + std::to_string(last) + " gave up after " +
             std::to_string(waited.count()) + " ms with serial " +
             std::to_string(runtime.completedSerial()) + " complete";
    }
    return {};
  };

  std::string siblingProblem;
  std::thread other([&] { siblingProblem = submitAndWait(sibling->runtimeDevice()); });
  const std::string rootProblem = submitAndWait(root->runtimeDevice());
  other.join();

  EXPECT_THAT(rootProblem, testing::IsEmpty());
  EXPECT_THAT(siblingProblem, testing::IsEmpty());
  EXPECT_FALSE(root->isDeviceLost());
}

// ---------------------------------------------------------------------------
// frameStampClaimed unit contract
// ---------------------------------------------------------------------------

TEST_F(GeodeSharedDeviceFrameTest, FrameStampClaimedTracksOpenGenerations) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  constexpr uint64_t kNeverDrawn = ~uint64_t{0};

  // No open frame: nothing is claimed, including the never-drawn sentinel,
  // which a naive `>=` against `oldestOpenFrameGeneration()`'s `~0` idle
  // value would misread as permanently claimed.
  EXPECT_FALSE(device->frameStampClaimed(kNeverDrawn));
  EXPECT_FALSE(device->frameStampClaimed(0));

  const uint64_t outer = device->beginFrameGeneration();
  // A stamp from the open frame is claimed; earlier (submitted) stamps and
  // the sentinel are not.
  EXPECT_TRUE(device->frameStampClaimed(outer));
  EXPECT_FALSE(device->frameStampClaimed(outer - 1));
  EXPECT_FALSE(device->frameStampClaimed(kNeverDrawn));

  // A nested (offscreen) generation: stamps from EITHER open frame are
  // claimed. This is the cross-renderer case an equality gate gets wrong -
  // `inner != outer` reads "free" while the outer frame's recorded draws
  // still depend on the stamped bytes.
  const uint64_t inner = device->beginFrameGeneration();
  EXPECT_TRUE(device->frameStampClaimed(outer));
  EXPECT_TRUE(device->frameStampClaimed(inner));

  device->endFrameGeneration(inner);
  // The inner frame submitted, the outer is still open: the outer stamp
  // stays claimed, and so does the inner one (it is newer than the oldest
  // open generation, so an open frame could still have recorded against
  // buffers it touched).
  EXPECT_TRUE(device->frameStampClaimed(outer));
  EXPECT_TRUE(device->frameStampClaimed(inner));

  device->endFrameGeneration(outer);
  EXPECT_FALSE(device->frameStampClaimed(outer));
  EXPECT_FALSE(device->frameStampClaimed(inner));
  EXPECT_FALSE(device->frameStampClaimed(kNeverDrawn));
}

// ---------------------------------------------------------------------------
// End-to-end: mid-frame offscreen render of the same document
// ---------------------------------------------------------------------------

/// Drives the outer renderer through the public `RendererInterface` surface
/// exactly as `RendererDriver::traverseRange` does (setPaint from the
/// instance's resolved fill, setTransform from the instance transform,
/// drawPath with the entity-bound `PathShape`), because `draw()` records and
/// submits atomically and the corruption window is BETWEEN those: the outer
/// frame records its gradient batch, an offscreen renderer of the same
/// document then draws mid-frame, and only afterwards does the outer frame
/// submit.
TEST_F(GeodeSharedDeviceFrameTest, MidFrameOffscreenRenderKeepsRecordedGradientPaint) {
  auto device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  SVGDocument document = parseDocument(kGradientRectSvg);
  Registry& registry = document.registry();
  RendererGeode outer(device);

  // Frame 1: a normal draw establishes residence, the entity's record slot,
  // and the gradient paint block, and proves the baseline pixels are blue.
  outer.draw(document);
  {
    const RendererBitmap baseline = outer.takeSnapshot();
    ASSERT_FALSE(baseline.empty());
    const auto px = PixelAt(baseline, 40, 40);
    ASSERT_TRUE(isBlue(px)) << "Baseline gradient rect must render blue, got rgba(" << int(px[0])
                            << "," << int(px[1]) << "," << int(px[2]) << "," << int(px[3]) << ")";
  }

  auto rectElement = document.querySelector("#target");
  ASSERT_TRUE(rectElement.has_value());
  const Entity rectEntity = rectElement->entityHandle().entity();
  const auto& instance = registry.get<components::RenderingInstanceComponent>(rectEntity);
  const auto& computedPath = registry.get<components::ComputedPathComponent>(rectEntity);

  // Frame 2, hand-driven: record the gradient rect the way the driver does.
  RenderViewport viewport;
  viewport.size = Vector2d(100.0, 100.0);
  viewport.devicePixelRatio = 1.0;
  outer.beginFrame(viewport);

  PaintParams gradientPaint;
  gradientPaint.fill = instance.resolvedFill;
  gradientPaint.fillOpacity = 1.0;
  gradientPaint.viewBox = Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0);
  outer.setPaint(gradientPaint);
  outer.setTransform(instance.worldFromEntityTransform);

  PathShape shape;
  shape.path = &computedPath.spline;
  shape.fillRule = FillRule::NonZero;
  shape.sourceEntity = EntityHandle(registry, rectEntity);
  outer.drawPath(shape, StrokeParams{});

  // Record the pending gradient batch into the outer frame's command stream
  // NOW: a solid non-entity draw flushes it (mirroring any later
  // state-changing draw in a real traversal). From here until `endFrame`,
  // the recorded batch draw depends on the entity's record slot and paint
  // block bytes.
  PaintParams cornerPaint;
  cornerPaint.fill = PaintServer::Solid(css::Color(css::RGBA(255, 255, 255, 255)));
  outer.setPaint(cornerPaint);
  outer.setTransform(Transform2d());
  outer.drawRect(Box2d::FromXYWH(90.0, 90.0, 8.0, 8.0), StrokeParams{});

  // Mid-frame: animate the gradient to green and render the same document
  // through an offscreen renderer on the same device (the feImage-fragment /
  // layer-thumbnail shape). Its fresh generation must see the outer frame's
  // claims: if it treats the slots as free it publishes green into the paint
  // block the outer frame's recorded draw reads at submit.
  {
    auto stop0 = document.querySelector("#s0");
    auto stop1 = document.querySelector("#s1");
    ASSERT_TRUE(stop0.has_value());
    ASSERT_TRUE(stop1.has_value());
    stop0->setAttribute("stop-color", "#00ff00");
    stop1->setAttribute("stop-color", "#00ff00");
  }
  std::unique_ptr<RendererInterface> inner = outer.createOffscreenInstance();
  ASSERT_NE(inner, nullptr);
  inner->draw(document);

  outer.endFrame();

  const RendererBitmap result = outer.takeSnapshot();
  ASSERT_FALSE(result.empty());
  const auto px = PixelAt(result, 40, 40);
  EXPECT_TRUE(isBlue(px)) << "Outer frame recorded the gradient as blue before the offscreen "
                             "render; a green pixel means the inner frame rewrote the paint "
                             "block the recorded draw reads at submit. Got rgba("
                          << int(px[0]) << "," << int(px[1]) << "," << int(px[2]) << ","
                          << int(px[3]) << ")";

  // The inner render itself must show the animated green (it drew last with
  // the updated document), proving the mid-frame render really painted G2 and
  // the outer's blue is preservation, not a stale inner pass.
  const RendererBitmap innerResult = static_cast<RendererGeode*>(inner.get())->takeSnapshot();
  ASSERT_FALSE(innerResult.empty());
  const auto innerPx = PixelAt(innerResult, 40, 40);
  EXPECT_TRUE(innerPx[1] > 200 && innerPx[0] < 60 && innerPx[2] < 60)
      << "Offscreen render must show the animated green gradient, got rgba(" << int(innerPx[0])
      << "," << int(innerPx[1]) << "," << int(innerPx[2]) << "," << int(innerPx[3]) << ")";
}

}  // namespace
}  // namespace donner::svg
