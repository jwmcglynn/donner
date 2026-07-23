#include "donner/svg/renderer/geode/GeodeTextureEncoder.h"

#include <cstdio>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "donner/svg/renderer/geode/GeodeImagePipeline.h"

namespace donner::geode {

namespace {

/// The donner::gpu runtime requires `bytesPerRow` to be 256-aligned for texture writes
/// (`gpu::kTexelRowPitchAlignment`). If the natural row stride isn't 256-aligned, rows are
/// copied through a padded staging buffer before upload.
constexpr uint32_t kBytesPerRowAlignment = 256u;

constexpr uint32_t alignUp(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

/// Layout of the per-draw uniform buffer (must match shaders/image_blit.wgsl).
///
/// 160 bytes total. `vec4f` members in WGSL require 16-byte alignment, so
/// `maskBounds` lands at offset 128 (not 120). The explicit 8-byte pad
/// before `maskBounds` mirrors that alignment so `blendMode` /
/// `hasClipMask` end up at the offsets the fragment shader reads from
/// (verified via `spirv-dis` on the naga-emitted SPIR-V - see
/// OpMemberDecorate offsets 128/144/148).
struct alignas(16) Uniforms {
  float mvp[16];                   //   0 ..  64
  float destRect[4];               //  64 ..  80
  float srcRect[4];                //  80 ..  96
  float targetSize[2];             //  96 .. 104 - target size for clip-mask UVs
  float opacity;                   // 104 .. 108
  uint32_t sourceIsPremult;        // 108 .. 112
  uint32_t maskMode;               // 112 .. 116 - Phase 3c <mask> luminance blit
  uint32_t applyMaskBounds;        // 116 .. 120 - clip output to `maskBounds`
  uint32_t _padBeforeMaskBounds0;  // 120 .. 124 - align maskBounds to vec4f (16B) boundary
  uint32_t _padBeforeMaskBounds1;  // 124 .. 128
  float maskBounds[4];             // 128 .. 144 - (x0, y0, x1, y1) in target-pixel space
  uint32_t blendMode;              // 144 .. 148 - Phase 3d mix-blend-mode selector
  uint32_t hasClipMask;            // 148 .. 152 - Phase 3b path-clip mask blit
  uint32_t _pad0;                  // 152 .. 156
  uint32_t _pad1;                  // 156 .. 160
};
static_assert(sizeof(Uniforms) == 160, "Image-blit Uniforms layout mismatch");

/// Logs a translation failure. Draw helpers fail closed by skipping the draw; the enclosing
/// command encoder's poisoned-latch (if the same root cause also poisoned recording) surfaces
/// at finish.
void LogGpuError(const char* what, const gpu::GpuError& error) {
  std::fprintf(stderr, "[Geode] %s failed: %s\n", what, error.message.c_str());
}

}  // namespace

gpu::Texture GeodeTextureEncoder::uploadRgba8Texture(const GeodeGpuContext& context,
                                                     const uint8_t* rgbaPixels, uint32_t width,
                                                     uint32_t height) {
  if (context.gpuDevice == nullptr || rgbaPixels == nullptr || width == 0u || height == 0u) {
    return gpu::Texture();
  }

  gpu::Result<gpu::Texture> textureResult = context.gpuDevice->createTexture(gpu::TextureDescriptor{
      "GeodeUploadedImage", gpu::Extent2d{width, height}, gpu::TextureFormat::RGBA8Unorm,
      gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst});
  if (textureResult.hasError()) {
    LogGpuError("GeodeUploadedImage createTexture", textureResult.error());
    return gpu::Texture();
  }
  gpu::Texture texture = std::move(textureResult).result();

  const uint32_t unpaddedBytesPerRow = width * 4u;
  const uint32_t paddedBytesPerRow = alignUp(unpaddedBytesPerRow, kBytesPerRowAlignment);
  const gpu::TexelCopyBufferLayout layout{0, paddedBytesPerRow, height};
  const gpu::Extent2d writeSize{width, height};

  gpu::Status writeStatus = gpu::OkStatus();
  if (unpaddedBytesPerRow == paddedBytesPerRow) {
    // Fast path - source rows already 256-aligned. Upload directly.
    const size_t byteCount = static_cast<size_t>(unpaddedBytesPerRow) * height;
    writeStatus = context.gpuDevice->writeTexture(
        texture, std::span<const uint8_t>(rgbaPixels, byteCount), layout, writeSize);
  } else {
    // Slow path - copy into a padded staging buffer. `std::vector` is the
    // allowed allocation path (stdlib, not raw malloc/free). We size-cap on
    // the same uint32_t x uint32_t x 4 that the GPU already accepted for
    // the texture, so overflow is not a concern here.
    std::vector<uint8_t> staging(static_cast<size_t>(paddedBytesPerRow) * height, 0u);
    for (uint32_t y = 0; y < height; ++y) {
      std::memcpy(staging.data() + static_cast<size_t>(y) * paddedBytesPerRow,
                  rgbaPixels + static_cast<size_t>(y) * unpaddedBytesPerRow, unpaddedBytesPerRow);
    }
    writeStatus = context.gpuDevice->writeTexture(
        texture, std::span<const uint8_t>(staging.data(), staging.size()), layout, writeSize);
  }
  if (writeStatus.hasError()) {
    LogGpuError("GeodeUploadedImage writeTexture", writeStatus.error());
    return gpu::Texture();
  }

  return texture;
}

void GeodeTextureEncoder::drawTexturedQuad(const GeodeGpuContext& context,
                                           const GeodeImagePipeline& pipeline,
                                           gpu::RenderPassEncoder& pass,
                                           const gpu::Texture& texture, const float mvp[16],
                                           uint32_t targetWidth, uint32_t targetHeight,
                                           const QuadParams& params,
                                           GeodeTransientResources& transients) {
  if (context.gpuDevice == nullptr || !texture.isValid()) {
    return;
  }
  gpu::Device& device = *context.gpuDevice;

  // Build uniform buffer.
  Uniforms u = {};
  std::memcpy(u.mvp, mvp, sizeof(u.mvp));
  u.destRect[0] = static_cast<float>(params.destRect.topLeft.x);
  u.destRect[1] = static_cast<float>(params.destRect.topLeft.y);
  u.destRect[2] = static_cast<float>(params.destRect.bottomRight.x);
  u.destRect[3] = static_cast<float>(params.destRect.bottomRight.y);
  u.srcRect[0] = static_cast<float>(params.srcRect.topLeft.x);
  u.srcRect[1] = static_cast<float>(params.srcRect.topLeft.y);
  u.srcRect[2] = static_cast<float>(params.srcRect.bottomRight.x);
  u.srcRect[3] = static_cast<float>(params.srcRect.bottomRight.y);
  u.targetSize[0] = static_cast<float>(targetWidth);
  u.targetSize[1] = static_cast<float>(targetHeight);
  u.opacity = static_cast<float>(params.opacity);
  u.sourceIsPremult = params.sourceIsPremultiplied ? 1u : 0u;
  u.maskMode = params.maskTexture != nullptr ? 1u : 0u;
  u.applyMaskBounds = params.applyMaskBounds ? 1u : 0u;
  u.maskBounds[0] = static_cast<float>(params.maskBounds.topLeft.x);
  u.maskBounds[1] = static_cast<float>(params.maskBounds.topLeft.y);
  u.maskBounds[2] = static_cast<float>(params.maskBounds.bottomRight.x);
  u.maskBounds[3] = static_cast<float>(params.maskBounds.bottomRight.y);
  u.blendMode = params.blendMode;
  u.hasClipMask = params.clipMaskView.isValid() ? 1u : 0u;

  gpu::Result<gpu::Buffer> uniformResult = device.createBuffer(
      gpu::BufferDescriptor{"GeodeImageBlitUniforms", sizeof(Uniforms),
                            gpu::BufferUsage::Uniform | gpu::BufferUsage::CopyDst});
  if (uniformResult.hasError()) {
    LogGpuError("GeodeImageBlitUniforms createBuffer", uniformResult.error());
    return;
  }
  const gpu::Buffer& uniformBuffer =
      transients.buffers.emplace_back(std::move(uniformResult).result());
  gpu::Status writeStatus = device.writeBuffer(
      uniformBuffer, 0,
      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&u), sizeof(Uniforms)));
  if (writeStatus.hasError()) {
    LogGpuError("GeodeImageBlitUniforms writeBuffer", writeStatus.error());
    return;
  }

  // Pick sampler based on requested filter mode.
  const gpu::Sampler& sampler =
      (params.filter == Filter::Nearest) ? pipeline.nearestSampler() : pipeline.linearSampler();

  // Bind group - optional texture bindings must always carry a valid
  // view. When their owning feature flags are off, we bind the content
  // view as a cheap dummy (the shader never samples it because the
  // corresponding mode guard is 0). References of the ONE created content
  // view are copied for the dummy slots - refs are copyable identities, so
  // no extra views are created.
  const auto makeView = [&](const gpu::Texture& viewed,
                            const char* what) -> const gpu::TextureView* {
    gpu::Result<gpu::TextureView> viewResult =
        device.createTextureView(viewed, gpu::TextureViewDescriptor{"GeodeImageBlitView"});
    if (viewResult.hasError()) {
      LogGpuError(what, viewResult.error());
      return nullptr;
    }
    return &transients.views.emplace_back(std::move(viewResult).result());
  };

  const gpu::TextureView* contentView =
      makeView(texture, "GeodeImageBlit content createTextureView");
  if (contentView == nullptr) {
    return;
  }
  gpu::TextureViewRef contentViewRef = *contentView;
  gpu::TextureViewRef maskViewRef = contentViewRef;
  if (params.maskTexture != nullptr) {
    const gpu::TextureView* maskView =
        makeView(*params.maskTexture, "GeodeImageBlit mask createTextureView");
    if (maskView == nullptr) {
      return;
    }
    maskViewRef = *maskView;
  }
  gpu::TextureViewRef dstViewRef = contentViewRef;
  if (params.dstSnapshotTexture != nullptr) {
    const gpu::TextureView* dstView =
        makeView(*params.dstSnapshotTexture, "GeodeImageBlit dstSnapshot createTextureView");
    if (dstView == nullptr) {
      return;
    }
    dstViewRef = *dstView;
  }
  const gpu::TextureViewRef clipMaskViewRef =
      params.clipMaskView.isValid() ? params.clipMaskView : contentViewRef;

  gpu::Result<gpu::BindGroup> bindGroupResult = device.createBindGroup(gpu::BindGroupDescriptor{
      "GeodeImageBlitBindGroup",
      pipeline.bindGroupLayout(),
      {
          gpu::BindGroupEntry{0, gpu::BufferBinding{uniformBuffer, 0, sizeof(Uniforms)}},
          gpu::BindGroupEntry{1, gpu::SamplerBinding{sampler}},
          gpu::BindGroupEntry{2, gpu::TextureViewBinding{contentViewRef}},
          gpu::BindGroupEntry{3, gpu::TextureViewBinding{maskViewRef}},
          gpu::BindGroupEntry{4, gpu::TextureViewBinding{dstViewRef}},
          gpu::BindGroupEntry{5, gpu::TextureViewBinding{clipMaskViewRef}},
          gpu::BindGroupEntry{6, gpu::SamplerBinding{pipeline.clipMaskSampler()}},
      }});
  if (bindGroupResult.hasError()) {
    LogGpuError("GeodeImageBlitBindGroup createBindGroup", bindGroupResult.error());
    return;
  }
  // NOTE: bind-group / buffer creates and buffer writes are counted by the adapter's
  // creation/write hooks (GeodeWgpuAdapterDevice), so no explicit count* calls here - counter
  // totals are unchanged from the pre-RHI recording path.
  const gpu::BindGroup& bindGroup =
      transients.bindGroups.emplace_back(std::move(bindGroupResult).result());

  // The caller is expected to have invoked `GeoEncoder::bindImagePipeline`
  // already, which is the sole place the image pipeline is bound + counted.
  // Set the pipeline here too as a defensive no-op (the backend collapses
  // the rebind); skip counting so `pipelineSwitches` reflects actual
  // pipeline transitions.
  pass.setPipeline(pipeline.pipeline());
  pass.setBindGroup(0, bindGroup);
  pass.draw(6, 1, 0, 0);
  context.countDraw();
}

}  // namespace donner::geode
