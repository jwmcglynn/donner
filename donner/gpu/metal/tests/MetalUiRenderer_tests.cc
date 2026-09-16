/// @file
/// UI draw data through the Metal backend: the shared scene renders exactly, proving the compiled
/// UI shader's packed-color unpacking, both alpha entry points, the device-pixel scissor and the
/// per-list indexed ranges execute as recorded.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>

#include "donner/editor/tests/UiDrawScene.h"
#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/metal/tests/MetalDeviceGate.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::metal::tests {
namespace {

class MetalUiRendererTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = MetalDevice::Create();
    DONNER_REQUIRE_METAL_DEVICE(device_, "the Metal UI rendering slice");
  }

  std::unique_ptr<MetalDevice> device_;
};

TEST_F(MetalUiRendererTest, SceneRendersExactly) {
  const auto readback = [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); };
  editor::tests::CheckUiScene(*device_, readback, "metal_ui_draw_scene");
}

}  // namespace
}  // namespace donner::gpu::metal::tests
