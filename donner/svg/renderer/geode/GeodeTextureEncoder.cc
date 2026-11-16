#include "donner/svg/renderer/geode/GeodeTextureEncoder.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "donner/gpu/Device.h"
#include "donner/gpu/shader/LinkedProjection.h"
#include "donner/gpu/shader/programs/ImageBlit.h"
#include "donner/svg/renderer/geode/GeodeAnchoredSampling.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodeNearestSampling.h"

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

using Uniforms = gpu::shader::programs::ImageBlitParams;

}  // namespace

gpu::Texture GeodeTextureEncoder::uploadRgba8Texture(const GeodeGpuContext& context,
                                                     const uint8_t* rgbaPixels, uint32_t width,
                                                     uint32_t height) {
  if (rgbaPixels == nullptr || width == 0u || height == 0u) {
    return gpu::Texture();
  }

  gpu::Result<gpu::Texture> created = context.gpuDevice->createTexture(gpu::TextureDescriptor{
      "GeodeUploadedImage", gpu::Extent2d{width, height}, gpu::TextureFormat::RGBA8Unorm,
      gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst});
  if (created.hasError()) {
    return gpu::Texture();
  }
  gpu::Texture texture = std::move(created).result();

  const uint32_t unpaddedBytesPerRow = width * 4u;
  const uint32_t paddedBytesPerRow = alignUp(unpaddedBytesPerRow, kBytesPerRowAlignment);
  const gpu::TexelCopyBufferLayout layout{0, paddedBytesPerRow, height};
  const gpu::Extent2d writeSize{width, height};

  if (unpaddedBytesPerRow == paddedBytesPerRow) {
    // Fast path - source rows already carry the required pitch. Upload directly.
    const size_t byteCount = static_cast<size_t>(unpaddedBytesPerRow) * height;
    (void)context.gpuDevice->writeTexture(texture, std::span<const uint8_t>(rgbaPixels, byteCount),
                                          layout, writeSize);
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
    (void)context.gpuDevice->writeTexture(texture, staging, layout, writeSize);
  }

  return texture;
}

namespace {

/// Fill the nearest-sampling rows that map target pixel positions to source texel coordinates:
/// the inverse of placing the quad's source texels in the target, narrowed and inverted in single
/// precision as the software renderer's image sampler does (see \ref NearestTexelRowsFor).
///
/// @param params Quad parameters.
/// @param textureSize Source texture dimensions in texels.
/// @param u Uniform block receiving the rows.
/// @return False when the placement is not invertible, so no pixel samples it.
/// The transform from source texel coordinates to target pixels for \p params.
Transform2d TargetFromTexel(const GeodeTextureEncoder::QuadParams& params,
                            const gpu::Extent2d& textureSize) {
  const Vector2d sourceSize(textureSize.width, textureSize.height);
  const Vector2d sourceTexelMin(params.srcRect.topLeft.x * sourceSize.x,
                                params.srcRect.topLeft.y * sourceSize.y);
  const Vector2d sourceTexelExtent(params.srcRect.width() * sourceSize.x,
                                   params.srcRect.height() * sourceSize.y);
  const Transform2d localFromTexel =
      Transform2d::Translate(-sourceTexelMin) *
      Transform2d::Scale(params.destRect.width() / sourceTexelExtent.x,
                         params.destRect.height() / sourceTexelExtent.y) *
      Transform2d::Translate(params.destRect.topLeft);
  return localFromTexel * params.targetFromLocal;
}

bool FillNearestTexelRows(const GeodeTextureEncoder::QuadParams& params,
                          const gpu::Extent2d& textureSize, Uniforms& u) {
  const std::optional<NearestTexelRows> rows =
      NearestTexelRowsFor(TargetFromTexel(params, textureSize));
  if (!rows.has_value()) {
    return false;
  }
  std::copy(rows->texelX.begin(), rows->texelX.end(), u.texelFromTargetX);
  std::copy(rows->texelY.begin(), rows->texelY.end(), u.texelFromTargetY);
  return true;
}

bool FillAnchoredTexelRows(const GeodeTextureEncoder::QuadParams& params,
                           const gpu::Extent2d& textureSize, Uniforms& u) {
  const std::optional<AnchoredTexelRows> rows =
      AnchoredTexelRowsFor(TargetFromTexel(params, textureSize), params.anchor);
  if (!rows.has_value()) {
    return false;
  }
  u.texelFromTargetX[0] = rows->linear[0];
  u.texelFromTargetX[1] = rows->linear[1];
  u.texelFromTargetX[2] = rows->fraction[0];
  u.texelFromTargetX[3] = static_cast<float>(rows->anchor.x);
  u.texelFromTargetY[0] = rows->linear[2];
  u.texelFromTargetY[1] = rows->linear[3];
  u.texelFromTargetY[2] = rows->fraction[1];
  u.texelFromTargetY[3] = static_cast<float>(rows->anchor.y);
  u.texelWholeX = rows->whole.x;
  u.texelWholeY = rows->whole.y;
  return true;
}

/// Fill the blit shader's uniform block for one quad.
///
/// @param mvp Column-major clip-from-target matrix.
/// @param targetWidth Render target width in texels.
/// @param targetHeight Render target height in texels.
/// @param params Quad parameters.
Uniforms BuildQuadUniforms(const float mvp[16], uint32_t targetWidth, uint32_t targetHeight,
                           const GeodeTextureEncoder::QuadParams& params) {
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
  u.maskMode = params.maskTexture != nullptr ? params.maskMode : 0u;
  u.applyMaskBounds = params.applyMaskBounds ? 1u : 0u;
  u.maskBounds[0] = static_cast<float>(params.maskBounds.topLeft.x);
  u.maskBounds[1] = static_cast<float>(params.maskBounds.topLeft.y);
  u.maskBounds[2] = static_cast<float>(params.maskBounds.bottomRight.x);
  u.maskBounds[3] = static_cast<float>(params.maskBounds.bottomRight.y);
  u.blendMode = params.blendMode;
  u.hasClipMask = params.clipMaskView != nullptr ? 1u : 0u;
  static_assert(static_cast<uint32_t>(GeodeTextureEncoder::Filter::Linear) == 0u &&
                    static_cast<uint32_t>(GeodeTextureEncoder::Filter::Nearest) == 1u &&
                    static_cast<uint32_t>(GeodeTextureEncoder::Filter::Pixelated) == 2u &&
                    static_cast<uint32_t>(GeodeTextureEncoder::Filter::Cubic) == 3u &&
                    static_cast<uint32_t>(GeodeTextureEncoder::Filter::Anchored) == 4u,
                "Filter values are the image-blit shader's samplingMode values");
  u.samplingMode = static_cast<uint32_t>(params.filter);
  u.pixelatedScale[0] = static_cast<float>(params.pixelatedScaleX);
  u.pixelatedScale[1] = static_cast<float>(params.pixelatedScaleY);
  return u;
}

/// The four texture views one quad's bind group needs.
///
/// Optional bindings must always carry a valid view: when their feature is off the source view is
/// bound as a stand-in, which the shader never samples because the matching mode guard is zero.
struct QuadViews {
  gpu::TextureViewRef source;
  gpu::TextureViewRef mask;
  gpu::TextureViewRef backdrop;
  gpu::TextureViewRef clipMask;
};

/// Open the views one quad binds, retaining each for the arena's lifetime.
///
/// @param device Device to create views through.
/// @param resourceArena Arena keeping the created views alive.
/// @param texture Source texture the quad samples.
/// @param params Quad parameters naming the optional mask, backdrop and clip mask.
/// @param outViews Receives the four views.
/// @return False when a view could not be created.
bool ResolveQuadViews(gpu::Device& device, GeodeTransientResources& resourceArena,
                      const gpu::Texture& texture, const GeodeTextureEncoder::QuadParams& params,
                      QuadViews& outViews) {
  const auto retainView = [&device, &resourceArena](const gpu::Texture& source, const char* label,
                                                    gpu::TextureViewRef& out) -> bool {
    gpu::Result<gpu::TextureView> created =
        device.createTextureView(source, gpu::TextureViewDescriptor{label});
    if (created.hasError()) {
      return false;
    }
    out = resourceArena.retain(std::move(created).result());
    return true;
  };

  if (!retainView(texture, "GeodeImageBlitView", outViews.source)) {
    return false;
  }
  outViews.mask = outViews.source;
  if (params.maskTexture != nullptr &&
      !retainView(*params.maskTexture, "GeodeImageBlitMaskView", outViews.mask)) {
    return false;
  }
  outViews.backdrop = outViews.source;
  if (params.dstSnapshotTexture != nullptr &&
      !retainView(*params.dstSnapshotTexture, "GeodeImageBlitBackdropView", outViews.backdrop)) {
    return false;
  }
  outViews.clipMask =
      params.clipMaskView != nullptr ? gpu::TextureViewRef(*params.clipMaskView) : outViews.source;
  return true;
}

/// Place one quad's uniforms in a buffer the draw can bind.
///
/// Pooled callers bump-allocate from their per-frame scratch arena; standalone callers get a
/// buffer of their own, retained for the arena's lifetime.
///
/// @param device Device to allocate through.
/// @param resourceArena Arena keeping a standalone buffer alive.
/// @param scratch Per-frame scratch arena, or null for a standalone buffer.
/// @param u Uniform block to place.
/// @param outBinding Receives the buffer, byte offset and byte size to bind.
/// @return False when the buffer could not be created.
bool AllocateQuadUniforms(gpu::Device& device, GeodeTransientResources& resourceArena,
                          GeodeTextureEncoder::UniformScratch* scratch, const Uniforms& u,
                          gpu::BufferBinding& outBinding) {
  if (scratch != nullptr) {
    constexpr uint64_t kUniformOffsetAlignment = GeodeTextureEncoder::kUniformOffsetAlignment;
    const GeodeTextureEncoder::UniformAllocation alloc =
        scratch->allocate(&u, sizeof(Uniforms), kUniformOffsetAlignment);
    outBinding = gpu::BufferBinding{alloc.buffer, alloc.offset, alloc.size};
    return true;
  }

  gpu::Result<gpu::Buffer> created = device.createBuffer(
      gpu::BufferDescriptor{"GeodeImageBlitUniforms", sizeof(Uniforms),
                            gpu::BufferUsage::Uniform | gpu::BufferUsage::CopyDst});
  if (created.hasError()) {
    return false;
  }
  const gpu::Buffer& retained = resourceArena.retain(std::move(created).result());
  (void)device.writeBuffer(
      retained, 0,
      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&u), sizeof(Uniforms)));
  outBinding = gpu::BufferBinding{retained, 0, sizeof(Uniforms)};
  return true;
}

}  // namespace

void GeodeTextureEncoder::BuildClipFromLocal(const Transform2d& targetFromLocal,
                                             uint32_t targetWidth, uint32_t targetHeight,
                                             float out16[16]) {
  // First: convert path-space -> pixel space via targetFromLocal.
  // Then: pixel space -> clip space.
  //   x_clip =  2 * x_pixel / width  - 1
  //   y_clip = -2 * y_pixel / height + 1   (Y flip for top-left origin)

  const double sx = 2.0 / static_cast<double>(targetWidth);
  const double sy = -2.0 / static_cast<double>(targetHeight);

  // Composed: clip = scale(sx, sy) * translate(-1, +1) * targetFromLocal
  // For affine 2D transforms in row-major mat4:
  //   [ a c 0 e ]   where targetFromLocal.data = [a b c d e f] (column-major 2x3)
  //   [ b d 0 f ]
  //   [ 0 0 1 0 ]
  //   [ 0 0 0 1 ]
  const double a = targetFromLocal.data[0];
  const double b = targetFromLocal.data[1];
  const double c = targetFromLocal.data[2];
  const double d = targetFromLocal.data[3];
  const double e = targetFromLocal.data[4];
  const double f = targetFromLocal.data[5];

  // Compose with the clip-space matrix.
  // Final mat4 (column-major for WGSL):
  //   col0 = (sx*a, sy*b, 0, 0)
  //   col1 = (sx*c, sy*d, 0, 0)
  //   col2 = (0, 0, 1, 0)
  //   col3 = (sx*e - 1, sy*f + 1, 0, 1)
  out16[0] = static_cast<float>(sx * a);
  out16[1] = static_cast<float>(sy * b);
  out16[2] = 0.0f;
  out16[3] = 0.0f;

  out16[4] = static_cast<float>(sx * c);
  out16[5] = static_cast<float>(sy * d);
  out16[6] = 0.0f;
  out16[7] = 0.0f;

  out16[8] = 0.0f;
  out16[9] = 0.0f;
  out16[10] = 1.0f;
  out16[11] = 0.0f;

  out16[12] = static_cast<float>(sx * e - 1.0);
  out16[13] = static_cast<float>(sy * f + 1.0);
  out16[14] = 0.0f;
  out16[15] = 1.0f;
}

namespace {

/// Fills the texel mapping that nearest and anchored sampling read from the uniforms. Returns
/// false when the mapping cannot be built, and true without changes for the other filters.
bool FillTexelMapping(gpu::Device& device, const gpu::Texture& texture, const float mvp[16],
                      uint32_t targetWidth, uint32_t targetHeight,
                      const GeodeTextureEncoder::QuadParams& params, Uniforms& u) {
  using Filter = GeodeTextureEncoder::Filter;
  if (params.filter != Filter::Nearest && params.filter != Filter::Anchored) {
    return true;
  }
#ifndef NDEBUG
  // Nearest and anchored sampling map pixels back through targetFromLocal, which must be the
  // mapping the MVP rasterizes the quad with.
  float expectedMvp[16];
  GeodeTextureEncoder::BuildClipFromLocal(params.targetFromLocal, targetWidth, targetHeight,
                                          expectedMvp);
  assert(std::equal(expectedMvp, expectedMvp + 16, mvp) &&
         "QuadParams::targetFromLocal must match the MVP");
#else
  (void)mvp;
  (void)targetWidth;
  (void)targetHeight;
#endif
  // A texture this device can draw always has a descriptor.
  const gpu::Result<gpu::TextureDescriptor> descriptor = device.textureDescriptor(texture);
  assert(descriptor.hasResult() && "texel mapping needs the source texture's dimensions");
  if (descriptor.hasError()) {
    return false;
  }
  return params.filter == Filter::Nearest
             ? FillNearestTexelRows(params, descriptor.result().size, u)
             : FillAnchoredTexelRows(params, descriptor.result().size, u);
}

}  // namespace

void GeodeTextureEncoder::drawTexturedQuad(
    const GeodeGpuContext& context, const GeodeImagePipeline& pipeline,
    gpu::RenderPassEncoder& pass, const gpu::Texture& texture, const float mvp[16],
    uint32_t targetWidth, uint32_t targetHeight, const QuadParams& params,
    GeodeTransientResources& resourceArena, UniformScratch* scratch) {
  if (!texture.isValid()) {
    return;
  }

  gpu::Device& device = *context.gpuDevice;

  Uniforms u = BuildQuadUniforms(mvp, targetWidth, targetHeight, params);
  if (!FillTexelMapping(device, texture, mvp, targetWidth, targetHeight, params, u)) {
    return;
  }
  gpu::BufferBinding uniformBinding;
  if (!AllocateQuadUniforms(device, resourceArena, scratch, u, uniformBinding)) {
    return;
  }

  // Pick sampler based on requested filter mode.
  const gpu::Sampler& sampler =
      params.filter == Filter::Nearest ? pipeline.nearestSampler() : pipeline.linearSampler();

  QuadViews views;
  if (!ResolveQuadViews(device, resourceArena, texture, params, views)) {
    return;
  }

  const auto& shader = DONNER_LINKED_SHADER_ARTIFACT(ImageBlit);
  gpu::Result<gpu::BindGroup> bindGroupResult = device.createBindGroup(gpu::BindGroupDescriptor{
      "GeodeImageBlitBindGroup",
      pipeline.bindGroupLayout(),
      {{shader.resource("uniforms")->binding, uniformBinding},
       {shader.resource("imageSampler")->binding, gpu::SamplerBinding{sampler}},
       {shader.resource("imageTexture")->binding, gpu::TextureViewBinding{views.source}},
       {shader.resource("maskTexture")->binding, gpu::TextureViewBinding{views.mask}},
       {shader.resource("dstSnapshotTexture")->binding, gpu::TextureViewBinding{views.backdrop}},
       {shader.resource("clipMaskTexture")->binding, gpu::TextureViewBinding{views.clipMask}}}});
  if (bindGroupResult.hasError()) {
    return;
  }
  const gpu::BindGroup& bindGroup = resourceArena.retain(std::move(bindGroupResult).result());

  // The caller is expected to have invoked `GeoEncoder::bindImagePipeline` already, which is the
  // sole place the image pipeline is bound + counted. Set the pipeline here too as a defensive
  // no-op; skip counting so `pipelineSwitches` reflects actual pipeline transitions.
  (void)pass.setPipeline(pipeline.pipeline());
  (void)pass.setBindGroup(0, bindGroup);
  (void)pass.draw(6, 1, 0, 0);
}

}  // namespace donner::geode
