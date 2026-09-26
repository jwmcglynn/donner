/// @file
/// A context over a natively selected Metal root counts the frame it renders. The counts come
/// only from the runtime device reporting to its context, so a context that stopped installing
/// its observer on a native device would read zero here.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <utility>

#include "donner/base/Box.h"
#include "donner/base/Vector2.h"
#include "donner/css/Color.h"
#include "donner/gpu/metal/tests/MetalDeviceGate.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/StrokeParams.h"
#include "donner/svg/renderer/geode/GeodeCounters.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeNativeRoot.h"

namespace donner::geode {
namespace {

using testing::Ge;
using testing::IsFalse;
using testing::NotNull;

TEST(GeodeNativeCountersTest, ANativeMetalContextCountsTheFrameItRenders) {
  GpuRootSelection selection;
  selection.label = "GeodeNativeCountersTest";
  selection.backend = GpuBackendKind::NativeMetal;
  std::shared_ptr<GeodeGpuRoot> root = SelectGpuRoot(selection);
  DONNER_REQUIRE_METAL_DEVICE(root, "native Metal counters");
  std::shared_ptr<GeodeDevice> device =
      GeodeDevice::CreateOverSelectedRoot(std::move(root), gpu::TextureFormat::RGBA8Unorm);
  ASSERT_THAT(device, NotNull()) << "a context over a selected native root must build";
  ASSERT_THAT(device->physicalDeviceOwner()->root().capabilities().backend,
              testing::Eq(GpuBackendKind::NativeMetal));

  svg::RendererGeode renderer(device);
  svg::RenderViewport viewport;
  viewport.size = Vector2d(64.0, 64.0);
  viewport.devicePixelRatio = 1.0;
  renderer.beginFrame(viewport);
  svg::PaintParams paint;
  paint.fill = svg::PaintServer::Solid{css::Color(css::RGBA(0, 255, 0, 255))};
  paint.fillOpacity = 1.0;
  paint.opacity = 1.0;
  renderer.setPaint(paint);
  renderer.drawRect(Box2d({8, 8}, {56, 56}), svg::StrokeParams{});
  renderer.endFrame();

  const GeodeCounters counters = renderer.lastFrameTimings().counters;
  EXPECT_THAT(renderer.deviceLost(), IsFalse());
  EXPECT_THAT(counters.submits, Ge(1u));
  EXPECT_THAT(counters.commandBuffers, Ge(1u));
  EXPECT_THAT(counters.drawCalls, Ge(1u));
  EXPECT_THAT(counters.bufferCreates, Ge(1u));
  EXPECT_THAT(counters.bindgroupCreates, Ge(1u));
  EXPECT_THAT(counters.textureCreates, Ge(1u));
}

}  // namespace
}  // namespace donner::geode
