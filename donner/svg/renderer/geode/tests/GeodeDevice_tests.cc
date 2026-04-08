#include "donner/svg/renderer/geode/GeodeDevice.h"

#include <gtest/gtest.h>

#include <string_view>

namespace donner::geode {

/// Smoke test: can we instantiate a headless Dawn device at all?
/// If this fails, the entire Geode backend is non-functional.
TEST(GeodeDevice, CreateHeadlessSucceeds) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr)
      << "Failed to create headless Dawn device. Check driver availability "
         "(Metal on macOS, Vulkan/SwiftShader on Linux).";

  EXPECT_TRUE(static_cast<bool>(device->device()));
  EXPECT_TRUE(static_cast<bool>(device->queue()));
  EXPECT_TRUE(static_cast<bool>(device->adapter()));
}

/// Can we allocate an offscreen render-target texture?
TEST(GeodeDevice, CanCreateRenderTargetTexture) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  wgpu::TextureDescriptor desc = {};
  desc.label = "TestRenderTarget";
  desc.size = {64, 64, 1};
  desc.format = wgpu::TextureFormat::RGBA8Unorm;
  desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  desc.mipLevelCount = 1;
  desc.sampleCount = 1;
  desc.dimension = wgpu::TextureDimension::e2D;

  wgpu::Texture texture = device->device().CreateTexture(&desc);
  ASSERT_TRUE(static_cast<bool>(texture));
  EXPECT_EQ(texture.GetWidth(), 64u);
  EXPECT_EQ(texture.GetHeight(), 64u);
}

/// Can we allocate a buffer for readback?
TEST(GeodeDevice, CanCreateReadbackBuffer) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  wgpu::BufferDescriptor desc = {};
  desc.label = "TestReadbackBuffer";
  desc.size = 1024;
  desc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;

  wgpu::Buffer buffer = device->device().CreateBuffer(&desc);
  ASSERT_TRUE(static_cast<bool>(buffer));
  EXPECT_EQ(buffer.GetSize(), 1024u);
}

/// End-to-end: clear a texture to red and read back the first pixel.
/// This proves that command submission and texture readback actually work.
TEST(GeodeDevice, CanExecuteClearAndReadback) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  const wgpu::Device& device = geodeDevice->device();
  const wgpu::Queue& queue = geodeDevice->queue();

  constexpr uint32_t kSize = 4;  // Small texture for a quick test.

  // Create render target.
  wgpu::TextureDescriptor texDesc = {};
  texDesc.size = {kSize, kSize, 1};
  texDesc.format = wgpu::TextureFormat::RGBA8Unorm;
  texDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  texDesc.mipLevelCount = 1;
  texDesc.sampleCount = 1;
  texDesc.dimension = wgpu::TextureDimension::e2D;
  wgpu::Texture target = device.CreateTexture(&texDesc);
  ASSERT_TRUE(static_cast<bool>(target));

  // Create readback buffer. Bytes per row must be a multiple of 256 per WebGPU spec.
  constexpr uint32_t kBytesPerRow = 256;  // Padded from kSize*4=16.
  constexpr uint32_t kBufferSize = kBytesPerRow * kSize;
  wgpu::BufferDescriptor bufDesc = {};
  bufDesc.size = kBufferSize;
  bufDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  wgpu::Buffer readback = device.CreateBuffer(&bufDesc);

  // Encode: clear to red, then copy to buffer.
  wgpu::CommandEncoder encoder = device.CreateCommandEncoder();

  wgpu::RenderPassColorAttachment colorAttachment = {};
  colorAttachment.view = target.CreateView();
  colorAttachment.loadOp = wgpu::LoadOp::Clear;
  colorAttachment.storeOp = wgpu::StoreOp::Store;
  colorAttachment.clearValue = {1.0, 0.0, 0.0, 1.0};  // Red.

  wgpu::RenderPassDescriptor passDesc = {};
  passDesc.colorAttachmentCount = 1;
  passDesc.colorAttachments = &colorAttachment;

  wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);
  pass.End();

  wgpu::TexelCopyTextureInfo src = {};
  src.texture = target;
  src.mipLevel = 0;
  src.origin = {0, 0, 0};

  wgpu::TexelCopyBufferInfo dst = {};
  dst.buffer = readback;
  dst.layout.bytesPerRow = kBytesPerRow;
  dst.layout.rowsPerImage = kSize;

  wgpu::Extent3D copySize = {kSize, kSize, 1};
  encoder.CopyTextureToBuffer(&src, &dst, &copySize);

  wgpu::CommandBuffer commands = encoder.Finish();
  queue.Submit(1, &commands);

  // Map the buffer synchronously via Tick loop.
  bool mapDone = false;
  readback.MapAsync(
      wgpu::MapMode::Read, 0, kBufferSize, wgpu::CallbackMode::AllowSpontaneous,
      [&mapDone](wgpu::MapAsyncStatus status, wgpu::StringView message) {
        EXPECT_EQ(status, wgpu::MapAsyncStatus::Success)
            << "Map failed: " << std::string_view(message.data, message.length);
        mapDone = true;
      });

  // Pump the Dawn event loop until the map completes.
  while (!mapDone) {
    device.Tick();
  }

  const uint8_t* pixels = static_cast<const uint8_t*>(readback.GetConstMappedRange());
  ASSERT_NE(pixels, nullptr);

  // First pixel should be red (255, 0, 0, 255).
  EXPECT_EQ(pixels[0], 255u) << "Red channel";
  EXPECT_EQ(pixels[1], 0u) << "Green channel";
  EXPECT_EQ(pixels[2], 0u) << "Blue channel";
  EXPECT_EQ(pixels[3], 255u) << "Alpha channel";

  readback.Unmap();
}

}  // namespace donner::geode
