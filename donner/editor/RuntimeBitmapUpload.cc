#include "donner/editor/RuntimeBitmapUpload.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "donner/gpu/GpuLimits.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"

namespace donner::editor {
namespace {
constexpr uint32_t kWgpuBytesPerRowAlignment = 256u;

uint32_t AlignWgpuBytesPerRow(uint32_t value) {
  return (value + kWgpuBytesPerRowAlignment - 1u) & ~(kWgpuBytesPerRowAlignment - 1u);
}

struct RuntimeBitmapUploadLayout {
  uint32_t allocationWidth = 0;
  uint32_t allocationHeight = 0;
  uint32_t bytesPerRow = 0;
  uint32_t payloadBytesPerRow = 0;
};

bool BitmapStorageCoversExtent(std::span<const uint8_t> pixels, std::size_t rowBytes,
                               uint32_t tightBytesPerRow, uint32_t height) {
  if (rowBytes < tightBytesPerRow) {
    return false;
  }
  if (height > 0u && rowBytes > std::numeric_limits<std::size_t>::max() / height) {
    return false;
  }
  return pixels.size() >= rowBytes * static_cast<std::size_t>(height);
}

bool BitmapDimensionsFitAllocation(Vector2i dimensions, Vector2i allocationDimensions) {
  return dimensions.x > 0 && dimensions.y > 0 && allocationDimensions.x >= dimensions.x &&
         allocationDimensions.y >= dimensions.y;
}

std::optional<RuntimeBitmapUploadLayout> ValidateRuntimeBitmapUpload(
    std::span<const uint8_t> pixels, Vector2i dimensions, std::size_t rowBytes,
    Vector2i allocationDimensions) {
  if (pixels.empty() || rowBytes == 0u ||
      !BitmapDimensionsFitAllocation(dimensions, allocationDimensions)) {
    return std::nullopt;
  }
  constexpr uint32_t kBytesPerPixel = 4u;
  const uint32_t width = static_cast<uint32_t>(dimensions.x);
  const uint32_t height = static_cast<uint32_t>(dimensions.y);
  const uint32_t allocationWidth = static_cast<uint32_t>(allocationDimensions.x);
  const uint32_t allocationHeight = static_cast<uint32_t>(allocationDimensions.y);
  if (width > std::numeric_limits<uint32_t>::max() / kBytesPerPixel ||
      allocationWidth > std::numeric_limits<uint32_t>::max() / kBytesPerPixel) {
    return std::nullopt;
  }
  const uint32_t tightBytesPerRow = width * kBytesPerPixel;
  const uint32_t allocationTightBytesPerRow = allocationWidth * kBytesPerPixel;
  if (allocationTightBytesPerRow >
      std::numeric_limits<uint32_t>::max() - (kWgpuBytesPerRowAlignment - 1u)) {
    return std::nullopt;
  }
  const uint32_t bytesPerRow = AlignWgpuBytesPerRow(allocationTightBytesPerRow);
  if (!BitmapStorageCoversExtent(pixels, rowBytes, tightBytesPerRow, height) ||
      (allocationHeight > 0u &&
       bytesPerRow > std::numeric_limits<std::size_t>::max() / allocationHeight)) {
    return std::nullopt;
  }

  return RuntimeBitmapUploadLayout{.allocationWidth = allocationWidth,
                                   .allocationHeight = allocationHeight,
                                   .bytesPerRow = bytesPerRow,
                                   .payloadBytesPerRow = tightBytesPerRow};
}

bool WriteRuntimeBitmapUpload(gpu::Device& device, const gpu::Texture& texture,
                              std::span<const uint8_t> pixels, Vector2i dimensions,
                              std::size_t rowBytes, const RuntimeBitmapUploadLayout& layout) {
  constexpr uint32_t kBytesPerPixel = 4u;
  constexpr std::size_t kMaxStagingBytes = 1024u * 1024u;
  static_assert(kMaxStagingBytes >=
                static_cast<std::size_t>(gpu::kMaxTextureDimension) * kBytesPerPixel);
  const uint32_t width = static_cast<uint32_t>(dimensions.x);
  const uint32_t height = static_cast<uint32_t>(dimensions.y);
  const uint32_t maxChunkRows = std::max<uint32_t>(1u, kMaxStagingBytes / layout.bytesPerRow);
  for (uint32_t firstRow = 0; firstRow < layout.allocationHeight;) {
    const uint32_t rowCount = std::min(maxChunkRows, layout.allocationHeight - firstRow);
    std::vector<uint8_t> staging(static_cast<std::size_t>(layout.bytesPerRow) * rowCount, 0u);
    for (uint32_t chunkRow = 0; chunkRow < rowCount; ++chunkRow) {
      const uint32_t destinationY = firstRow + chunkRow;
      if (destinationY > height) {
        continue;
      }
      const uint32_t sourceY = std::min(destinationY, height - 1u);
      const uint8_t* sourceRow = pixels.data() + static_cast<std::size_t>(sourceY) * rowBytes;
      uint8_t* destinationRow =
          staging.data() + static_cast<std::size_t>(chunkRow) * layout.bytesPerRow;
      std::memcpy(destinationRow, sourceRow, layout.payloadBytesPerRow);
      if (layout.allocationWidth > width) {
        std::memcpy(destinationRow + layout.payloadBytesPerRow,
                    sourceRow + static_cast<std::size_t>(width - 1u) * kBytesPerPixel,
                    kBytesPerPixel);
      }
    }
    // Device::writeTexture consumes the byte span during the call, so the next chunk can reuse
    // this bounded staging allocation without extending its lifetime through submission.
    if (device
            .writeTexture(
                texture, staging, gpu::TexelCopyBufferLayout{0u, layout.bytesPerRow, rowCount},
                gpu::Extent2d{layout.allocationWidth, rowCount}, gpu::Origin2d{0u, firstRow})
            .hasError()) {
      return false;
    }
    firstRow += rowCount;
  }
  return true;
}

std::shared_ptr<svg::RendererGeodeTextureSnapshot> AcquireRuntimeUploadSnapshot(
    const std::shared_ptr<geode::GeodeDevice>& device, Vector2i dimensions,
    svg::AlphaType alphaType, Vector2i allocationDimensions) {
  gpu::Result<gpu::Texture> texture = device->runtimeDevice().createTexture(gpu::TextureDescriptor{
      "EditorUploadedBitmap",
      {static_cast<uint32_t>(allocationDimensions.x),
       static_cast<uint32_t>(allocationDimensions.y)},
      gpu::TextureFormat::RGBA8Unorm,
      gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst | gpu::TextureUsage::CopySrc});
  if (texture.hasError()) {
    return nullptr;
  }
  svg::RendererGeodeTextureSnapshot snapshot =
      svg::RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
          device, std::move(texture).result(), dimensions, wgpu::TextureFormat::RGBA8Unorm,
          alphaType);
  if (!snapshot.isValid()) {
    return nullptr;
  }
  return std::make_shared<svg::RendererGeodeTextureSnapshot>(std::move(snapshot));
}

}  // namespace

std::shared_ptr<svg::RendererGeodeTextureSnapshot> UploadRuntimeBitmap(
    const std::shared_ptr<geode::GeodeDevice>& device, std::span<const uint8_t> pixels,
    Vector2i dimensions, std::size_t rowBytes, svg::AlphaType alphaType,
    Vector2i allocationDimensions) {
  if (device == nullptr) {
    return nullptr;
  }
  const std::optional<RuntimeBitmapUploadLayout> layout =
      ValidateRuntimeBitmapUpload(pixels, dimensions, rowBytes, allocationDimensions);
  if (!layout.has_value()) {
    return nullptr;
  }
  std::shared_ptr<svg::RendererGeodeTextureSnapshot> uploaded =
      AcquireRuntimeUploadSnapshot(device, dimensions, alphaType, allocationDimensions);
  if (uploaded == nullptr) {
    return nullptr;
  }
  const gpu::Texture* runtimeTexture = uploaded->runtimeTexture();
  if (runtimeTexture == nullptr ||
      !WriteRuntimeBitmapUpload(device->runtimeDevice(), *runtimeTexture, pixels, dimensions,
                                rowBytes, *layout)) {
    return nullptr;
  }
  if (!uploaded->setDimensions(dimensions)) {
    return nullptr;
  }
  return uploaded;
}

}  // namespace donner::editor
