#include "donner/svg/renderer/geode/GeodeTextureEncoder.h"

#include <cstring>
#include <vector>

#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::geode {

namespace {

/// WebGPU requires `bytesPerRow` to be 256-aligned when copying buffer → texture.
/// `queue.writeTexture` accepts unaligned rows only on some backends, so we
/// normalize: if the natural row stride isn't 256-aligned, copy through a
/// padded staging buffer before upload.
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
/// (verified via `spirv-dis` on the naga-emitted SPIR-V — see
/// OpMemberDecorate offsets 128/144/148).
struct alignas(16) Uniforms {
  float mvp[16];                   //   0 ..  64
  float destRect[4];               //  64 ..  80
  float srcRect[4];                //  80 ..  96
  float targetSize[2];             //  96 .. 104 — target size for clip-mask UVs
  float opacity;                   // 104 .. 108
  uint32_t sourceIsPremult;        // 108 .. 112
  uint32_t maskMode;               // 112 .. 116 — Phase 3c <mask> luminance blit
  uint32_t applyMaskBounds;        // 116 .. 120 — clip output to `maskBounds`
  uint32_t _padBeforeMaskBounds0;  // 120 .. 124 — align maskBounds to vec4f (16B) boundary
  uint32_t _padBeforeMaskBounds1;  // 124 .. 128
  float maskBounds[4];             // 128 .. 144 — (x0, y0, x1, y1) in target-pixel space
  uint32_t blendMode;              // 144 .. 148 — Phase 3d mix-blend-mode selector
  uint32_t hasClipMask;            // 148 .. 152 — Phase 3b path-clip mask blit
  uint32_t _pad0;                  // 152 .. 156
  uint32_t _pad1;                  // 156 .. 160
};
static_assert(sizeof(Uniforms) == 160, "Image-blit Uniforms layout mismatch");

}  // namespace

wgpu::Texture GeodeTextureEncoder::uploadRgba8Texture(GeodeDevice& device,
                                                      const uint8_t* rgbaPixels, uint32_t width,
                                                      uint32_t height) {
  if (rgbaPixels == nullptr || width == 0u || height == 0u) {
    return wgpu::Texture();
  }

  const wgpu::Device& dev = device.device();
  const wgpu::Queue& queue = device.queue();

  wgpu::TextureDescriptor td = {};
  td.label = wgpuLabel("GeodeUploadedImage");
  td.size = {width, height, 1};
  td.format = wgpu::TextureFormat::RGBA8Unorm;
  td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  td.dimension = wgpu::TextureDimension::_2D;
  wgpu::Texture texture = dev.createTexture(td);
  if (!texture) {
    return wgpu::Texture();
  }
  device.countTexture();

  const uint32_t unpaddedBytesPerRow = width * 4u;
  const uint32_t paddedBytesPerRow = alignUp(unpaddedBytesPerRow, kBytesPerRowAlignment);

  wgpu::TexelCopyTextureInfo dst = {};
  dst.texture = texture;
  dst.mipLevel = 0;
  dst.origin = {0, 0, 0};
  dst.aspect = wgpu::TextureAspect::All;

  wgpu::TexelCopyBufferLayout layout = {};
  layout.offset = 0;
  layout.bytesPerRow = paddedBytesPerRow;
  layout.rowsPerImage = height;

  wgpu::Extent3D writeSize = {width, height, 1};

  if (unpaddedBytesPerRow == paddedBytesPerRow) {
    // Fast path — source rows already 256-aligned. Upload directly.
    const size_t byteCount = static_cast<size_t>(unpaddedBytesPerRow) * height;
    queue.writeTexture(dst, rgbaPixels, byteCount, layout, writeSize);
  } else {
    // Slow path — copy into a padded staging buffer. Reuse a thread-
    // local buffer across calls so the compositor's per-frame
    // `drawImage` invocations (one per cached layer bitmap) don't each
    // pay a 7 MB-scale `std::vector` allocate + zero-init + destroy
    // tax. On the splash at 1784×1024, `paddedBytesPerRow` is 7168 and
    // the old `std::vector<uint8_t>(…, 0u)` constructor was ~40 % of
    // every worker-thread `composeLayers` frame. The thread-local
    // survives across frames within the worker and only grows.
    //
    // We memcpy every row into the buffer below, so the padding bytes
    // don't need to be zeroed — the GPU ignores the bytes after
    // `unpaddedBytesPerRow` on each row. That lets us `resize`
    // without a fill value even when we grow the vector.
    thread_local std::vector<uint8_t> staging;
    const size_t needed = static_cast<size_t>(paddedBytesPerRow) * height;
    if (staging.size() < needed) {
      staging.resize(needed);
    }
    for (uint32_t y = 0; y < height; ++y) {
      std::memcpy(staging.data() + static_cast<size_t>(y) * paddedBytesPerRow,
                  rgbaPixels + static_cast<size_t>(y) * unpaddedBytesPerRow, unpaddedBytesPerRow);
    }
    queue.writeTexture(dst, staging.data(), needed, layout, writeSize);
  }

  return texture;
}

void GeodeTextureEncoder::drawTexturedQuad(GeodeDevice& device, const GeodeImagePipeline& pipeline,
                                           const wgpu::RenderPassEncoder& pass,
                                           const wgpu::Texture& texture, const float mvp[16],
                                           uint32_t targetWidth, uint32_t targetHeight,
                                           const QuadParams& params) {
  if (!texture) {
    return;
  }

  const wgpu::Device& dev = device.device();
  const wgpu::Queue& queue = device.queue();

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
  u.maskMode = params.maskTexture ? 1u : 0u;
  u.applyMaskBounds = params.applyMaskBounds ? 1u : 0u;
  u.maskBounds[0] = static_cast<float>(params.maskBounds.topLeft.x);
  u.maskBounds[1] = static_cast<float>(params.maskBounds.topLeft.y);
  u.maskBounds[2] = static_cast<float>(params.maskBounds.bottomRight.x);
  u.maskBounds[3] = static_cast<float>(params.maskBounds.bottomRight.y);
  u.blendMode = params.blendMode;
  u.hasClipMask = params.clipMaskView ? 1u : 0u;

  wgpu::BufferDescriptor uniDesc = {};
  uniDesc.label = wgpuLabel("GeodeImageBlitUniforms");
  uniDesc.size = sizeof(Uniforms);
  uniDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer uniBuf = dev.createBuffer(uniDesc);
  device.countBuffer();
  queue.writeBuffer(uniBuf, 0, &u, sizeof(Uniforms));

  // Pick sampler based on requested filter mode.
  const wgpu::Sampler& sampler =
      (params.filter == Filter::Nearest) ? pipeline.nearestSampler() : pipeline.linearSampler();

  // Bind group — optional texture bindings must always carry a valid
  // view. When their owning feature flags are off, we bind the source
  // content texture view as a cheap dummy (the shader never samples it
  // because the corresponding mode guard is 0).
  wgpu::TextureView view = texture.createView();
  wgpu::TextureView maskView = params.maskTexture ? params.maskTexture.createView() : view;
  wgpu::TextureView dstView =
      params.dstSnapshotTexture ? params.dstSnapshotTexture.createView() : view;
  wgpu::TextureView clipMaskView = params.clipMaskView ? params.clipMaskView : view;
  wgpu::BindGroupEntry entries[7] = {};
  entries[0].binding = 0;
  entries[0].buffer = uniBuf;
  entries[0].size = sizeof(Uniforms);
  entries[1].binding = 1;
  entries[1].sampler = sampler;
  entries[2].binding = 2;
  entries[2].textureView = view;
  entries[3].binding = 3;
  entries[3].textureView = maskView;
  entries[4].binding = 4;
  entries[4].textureView = dstView;
  entries[5].binding = 5;
  entries[5].textureView = clipMaskView;
  entries[6].binding = 6;
  entries[6].sampler = pipeline.clipMaskSampler();

  wgpu::BindGroupDescriptor bgDesc = {};
  bgDesc.label = wgpuLabel("GeodeImageBlitBindGroup");
  bgDesc.layout = pipeline.bindGroupLayout();
  bgDesc.entryCount = 7;
  bgDesc.entries = entries;
  wgpu::BindGroup bindGroup = dev.createBindGroup(bgDesc);
  device.countBindGroup();

  // The caller is expected to have invoked `GeoEncoder::bindImagePipeline`
  // already, which is the sole place the image pipeline is bound + counted.
  // Set the pipeline here too as a defensive no-op (Dawn collapses the
  // rebind on GPU); skip counting so `pipelineSwitches` reflects actual
  // pipeline transitions.
  pass.setPipeline(pipeline.pipeline());
  pass.setBindGroup(0, bindGroup, 0, nullptr);
  pass.draw(6, 1, 0, 0);
  device.countDraw();
}

}  // namespace donner::geode
