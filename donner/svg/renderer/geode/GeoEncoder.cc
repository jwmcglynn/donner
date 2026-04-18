#include "donner/svg/renderer/geode/GeoEncoder.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodePathEncoder.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/renderer/geode/GeodeTextureEncoder.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"
#include "donner/svg/resources/ImageResource.h"

namespace donner::geode {

namespace {

/// Round size up to a multiple of 4 (WebGPU requires buffer sizes to be
/// multiples of 4 for COPY_SRC/COPY_DST operations).
constexpr uint64_t roundUp4(uint64_t size) { return (size + 3u) & ~uint64_t{3u}; }

/// Layout of the per-draw uniform buffer (must match shaders/slug_fill.wgsl).
///
/// WGSL struct layout requires the total size to be a multiple of the largest
/// member alignment. With two mat4x4 members (16-byte alignment) the struct
/// rounds to 176 bytes. We pad explicitly to keep this stable across
/// compilers / WGSL backends.
///
/// Field order must stay in lock-step with the WGSL `Uniforms` struct.
struct alignas(16) Uniforms {
  float mvp[16];              //   0 ..  64
  float patternFromPath[16];  //  64 .. 128
  float viewport[2];          // 128 .. 136
  float tileSize[2];          // 136 .. 144
  float color[4];             // 144 .. 160
  uint32_t fillRule;          // 160 .. 164
  uint32_t paintMode;         // 164 .. 168
  float patternOpacity;       // 168 .. 172
  uint32_t hasClipPolygon;    // 172 .. 176 — 0 = no clip, 1 = clipPolygon active
  // Phase 3b path-clip mask flag. When nonzero, the shader samples the
  // clip mask texture at binding 5 (linear-filtered R8Unorm) and folds
  // its red-channel coverage into the fragment colour. A 1x1 dummy
  // texture is always bound so the `textureSample` is always legal.
  uint32_t hasClipMask;       // 176 .. 180
  uint32_t _clipPad0;         // 180 .. 184 — std140 alignment for next vec4 array
  uint32_t _clipPad1;         // 184 .. 188
  uint32_t _clipPad2;         // 188 .. 192
  // Phase 3a polygon clipping: a 4-vertex convex clip polygon expressed
  // as 4 edge half-planes, one per side, in VIEWPORT-PIXEL space. Each
  // edge is `(a, b, c)` such that `a*x + b*y + c >= 0` marks the inside
  // half-plane (the normal `(a, b)` points into the clipped region).
  // The fragment shader AND's these half-plane tests into its
  // `sample_mask` output so the clip integrates with the per-sample
  // MSAA coverage path. Used by `RendererGeode::pushClip` for
  // transformed rectangular viewports (`<symbol>` / `<use>` /
  // `<svg>` viewports with a non-axis-aligned transform) where the
  // true clip shape is a parallelogram that WebGPU's rectangular
  // scissor rect cannot express. Stored as `vec4f[4]` (vec4 = xyz + pad)
  // so the struct stays mat4x4-aligned and the WGSL side reads
  // `array<vec4f, 4>` directly.
  float clipPolygonPlanes[16];  // 192 .. 256 (4 edges × vec4)
};
static_assert(sizeof(Uniforms) == 256, "Uniforms struct layout mismatch");

/// Build a column-major 4x4 matrix from an affine `Transform2d` and write it
/// into the first 16 floats of the output array. Used for the `mvp` and
/// `patternFromPath` uniform fields — both expect mat4x4 layout even though
/// the 2D content only needs a 2x3 affine.
void affineToMat4(const Transform2d& t, float* out16) {
  const double a = t.data[0];
  const double b = t.data[1];
  const double c = t.data[2];
  const double d = t.data[3];
  const double e = t.data[4];
  const double f = t.data[5];
  // col0
  out16[0] = static_cast<float>(a);
  out16[1] = static_cast<float>(b);
  out16[2] = 0.0f;
  out16[3] = 0.0f;
  // col1
  out16[4] = static_cast<float>(c);
  out16[5] = static_cast<float>(d);
  out16[6] = 0.0f;
  out16[7] = 0.0f;
  // col2
  out16[8] = 0.0f;
  out16[9] = 0.0f;
  out16[10] = 1.0f;
  out16[11] = 0.0f;
  // col3
  out16[12] = static_cast<float>(e);
  out16[13] = static_cast<float>(f);
  out16[14] = 0.0f;
  out16[15] = 1.0f;
}

/// Must match `kMaxStops` in `shaders/slug_gradient.wgsl`.
constexpr uint32_t kMaxGradientStops = 16u;

/// Layout of the gradient per-draw uniform buffer. Must match
/// `GradientUniforms` in `shaders/slug_gradient.wgsl`.
///
/// Layout (offsets / sizes):
///   mvp                  64 bytes  [  0 ..  64]
///   viewport             8         [ 64 ..  72]
///   fillRule             4         [ 72 ..  76]
///   spreadMode           4         [ 76 ..  80]
///   row0                 16        [ 80 ..  96]
///   row1                 16        [ 96 .. 112]
///   startGrad            8         [112 .. 120]  (linear)
///   endGrad              8         [120 .. 128]  (linear)
///   radialCenter         8         [128 .. 136]  (radial)
///   radialFocal          8         [136 .. 144]  (radial)
///   radialRadius         4         [144 .. 148]  (radial)
///   radialFocalRadius    4         [148 .. 152]  (radial)
///   gradientKind         4         [152 .. 156]
///   stopCount            4         [156 .. 160]
///   stopColors           16 * 16   [160 .. 416]
///   stopOffsets           4 * 16   [416 .. 480]
///
/// Total: 480 bytes, a multiple of 16.
struct alignas(16) GradientUniforms {
  float mvp[16];              // 0   .. 64
  float viewport[2];          // 64  .. 72
  uint32_t fillRule;          // 72  .. 76
  uint32_t spreadMode;        // 76  .. 80
  float row0[4];              // 80  .. 96
  float row1[4];              // 96  .. 112
  float startGrad[2];         // 112 .. 120
  float endGrad[2];           // 120 .. 128
  float radialCenter[2];      // 128 .. 136
  float radialFocal[2];       // 136 .. 144
  float radialRadius;         // 144 .. 148
  float radialFocalRadius;    // 148 .. 152
  uint32_t gradientKind;      // 152 .. 156
  uint32_t stopCount;         // 156 .. 160
  float stopColors[16 * 4];   // 160 .. 416
  float stopOffsets[4 * 4];   // 416 .. 480
  // Phase 3a convex clip polygon + Phase 3b path-clip mask flag.
  // Layout mirrors `slug_gradient.wgsl` — `hasClipPolygon` +
  // `hasClipMask` + 2 pad u32 to reach vec4 alignment, then the 4
  // half-plane rows.
  uint32_t hasClipPolygon;    // 480 .. 484
  uint32_t hasClipMask;       // 484 .. 488
  uint32_t _clipPad1;         // 488 .. 492
  uint32_t _clipPad2;         // 492 .. 496
  float clipPolygonPlanes[16];// 496 .. 560
};
static_assert(sizeof(GradientUniforms) == 560,
              "GradientUniforms struct layout mismatch");

/// Gradient kind values shared with `shaders/slug_gradient.wgsl`.
constexpr uint32_t kGradientKindLinear = 0u;
constexpr uint32_t kGradientKindRadial = 1u;

}  // namespace

struct GeoEncoder::Impl {
  GeodeDevice* device;
  const GeodePipeline* pipeline;
  const GeodeGradientPipeline* gradientPipeline;
  const GeodeImagePipeline* imagePipeline;
  // 4× multisampled color attachment. All draws land here and the
  // hardware resolves into `target` at the end of each render pass.
  wgpu::Texture msaaTarget;
  wgpu::TextureView msaaTargetView;
  // 1-sample resolve texture. External code (image blit, readback)
  // samples / copies from this texture, never the MSAA color.
  wgpu::Texture target;
  wgpu::TextureView targetView;
  wgpu::CommandEncoder commandEncoder;
  uint32_t targetWidth;
  uint32_t targetHeight;

  // Dummy 1x1 texture + sampler bound when `paintMode == 0` (solid fill).
  // The bind group layout always requires texture/sampler entries so the
  // pipeline can be shared between solid and pattern fills, but in solid
  // mode the shader never reads from them. Lazily initialised on first use
  // because the device may create the encoder before any draw is issued.
  wgpu::Texture dummyTexture;
  wgpu::TextureView dummyTextureView;
  wgpu::Sampler dummySampler;

  // 1x1 R8Unorm dummy texture bound to the clip-mask slot when no
  // clip is active. The single texel is `0xFF` so `textureSample(...).r`
  // returns 1.0 — i.e., "this pixel is fully unclipped" — allowing
  // the shader to sample unconditionally without branching on
  // `hasClipMask` just to avoid an invalid texture read.
  wgpu::Texture dummyClipMaskTexture;
  wgpu::TextureView dummyClipMaskTextureView;

  // Currently-bound clip mask state (Phase 3b). When
  // `activeClipMaskView` is non-null, `hasClipMask == 1` in the
  // uniforms and draws sample `activeClipMaskView` through the
  // clip-mask binding. When null, the dummy is bound instead.
  wgpu::TextureView activeClipMaskView;
  wgpu::Sampler clipMaskSampler;

  // Lazily-constructed mask-rendering pipeline. We build one when the
  // first `beginMaskPass` call arrives so encoders that never touch
  // clipping pay no construction cost. Shared across all mask passes
  // within the lifetime of this encoder.
  std::unique_ptr<GeodeMaskPipeline> maskPipelineOwned;

  // While a mask pass is open (`maskPassOpen == true`), the main
  // render pass is closed — draw calls that hit the mask pipeline go
  // through `maskPass`. `beginMaskPass` saves the current `transform`
  // so main-pass draw code picks back up exactly where it left off
  // when the mask pass ends.
  bool maskPassOpen = false;
  wgpu::RenderPassEncoder maskPass;
  // Transform active when the mask pass was opened, so mask draws use
  // the same device-pixel space as the parent content. The mask pass
  // always renders into the mask texture the caller passed in, which
  // is the same size as the main target.
  Transform2d maskPassSavedTransform = Transform2d();

  // Pending draws are recorded into a render pass that's lazily opened.
  // The first clear/fill triggers `beginPass()`; finish() ends it.
  bool passOpen = false;
  wgpu::RenderPassEncoder pass;

  // Default load op = clear-to-transparent until clear() is called explicitly.
  wgpu::Color clearColor = {0.0, 0.0, 0.0, 0.0};
  bool hasExplicitClear = false;
  // When true, the next render pass uses LoadOp::Load so previously
  // submitted content is preserved. Set via `setLoadPreserve()`.
  bool loadPreserve = false;

  // Current transform — applied to MVP for the next draw.
  Transform2d transform = Transform2d();  // Identity.

  // Current scissor rectangle in target-pixel coords. An empty/unset
  // scissor means "no clipping" (full target extent). Applied to each
  // render pass as it opens via `SetScissorRect` — subsequent pushClip /
  // popClip updates also re-apply during the currently-active pass.
  //
  // `scissorActive == false` means the scissor has never been set OR was
  // popped back to the default, and draws should rasterize into the full
  // target. `scissorActive == true` means the current scissor is
  // `scissorX,scissorY,scissorW,scissorH` (all in target-pixel units).
  bool scissorActive = false;
  uint32_t scissorX = 0;
  uint32_t scissorY = 0;
  uint32_t scissorW = 0;
  uint32_t scissorH = 0;

  /// Phase 3a polygon clipping state. When `clipPolygonActive` is true,
  /// the 4 planes in `clipPolygonPlanes` describe the inside half-plane
  /// of each edge of a convex 4-vertex clip polygon in VIEWPORT-PIXEL
  /// space. Each plane is `(a, b, c)` such that a fragment at
  /// `@builtin(position).xy` is inside when `a*x + b*y + c >= 0`. The
  /// fragment shader AND's these tests into its sample_mask so the
  /// clip integrates with per-sample MSAA coverage.
  ///
  /// Set via `setClipPolygon` from `RendererGeode::pushClip` when the
  /// current clip is a rectangular viewport with a non-axis-aligned
  /// ancestor transform (where WebGPU's scissor rect can only describe
  /// the AABB of the transformed rect, not the true parallelogram).
  /// Cleared via `clearClipPolygon` when popClip restores a clip with
  /// no active polygon.
  bool clipPolygonActive = false;
  float clipPolygonPlanes[16] = {0};  // 4 edges × vec4 (xyz + pad)

  /// Populate the `hasClipPolygon` + `clipPolygonPlanes` fields on an
  /// outgoing `Uniforms` / `GradientUniforms` struct. Keeps the
  /// encoding of the clip state centralised so every draw helper that
  /// writes a uniform picks up the same snapshot.
  void writeClipPolygonUniforms(uint32_t& outFlag, float (&outPlanes)[16]) const {
    outFlag = clipPolygonActive ? 1u : 0u;
    for (size_t i = 0; i < 16; ++i) {
      outPlanes[i] = clipPolygonPlanes[i];
    }
  }

  /// Apply the current scissor to the open render pass. No-op if the
  /// pass isn't open yet — `ensurePassOpen` will call this on first
  /// open. Safe to call whenever `scissorActive` / `scissor*` changes.
  void applyScissorIfPassOpen() {
    if (!passOpen) {
      return;
    }
    if (scissorActive) {
      // Clamp to the target so out-of-bounds scissor rects don't trigger
      // WebGPU validation errors. Required when the clip rect is outside
      // the current viewport (e.g., a nested SVG positioned at the edge).
      uint32_t x = std::min(scissorX, targetWidth);
      uint32_t y = std::min(scissorY, targetHeight);
      uint32_t maxW = targetWidth - x;
      uint32_t maxH = targetHeight - y;
      uint32_t w = std::min(scissorW, maxW);
      uint32_t h = std::min(scissorH, maxH);
      pass.setScissorRect(x, y, w, h);
    } else {
      pass.setScissorRect(0, 0, targetWidth, targetHeight);
    }
  }

  /// Lazily create the dummy texture + sampler used by the solid-fill path
  /// *and* the Phase 3b dummy clip mask texture + clip mask sampler.
  void ensureDummyResources() {
    if (dummyTextureView) {
      return;
    }
    const wgpu::Device& dev = device->device();

    // --- Pattern dummy (RGBA8Unorm, 1x1, opaque black) ---
    wgpu::TextureDescriptor td = {};
    td.label = wgpuLabel("GeoEncoderDummyPattern");
    td.size = {1u, 1u, 1u};
    td.format = wgpu::TextureFormat::RGBA8Unorm;
    td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.dimension = wgpu::TextureDimension::_2D;
    dummyTexture = dev.createTexture(td);

    // Write a single opaque-black pixel so sampling returns a defined value
    // even though the shader never reads it in solid mode. WriteTexture
    // allows unpadded rows when the transfer fits in a single row.
    const uint8_t pixel[4] = {0, 0, 0, 255};
    wgpu::TexelCopyTextureInfo dst = {};
    dst.texture = dummyTexture;
    wgpu::TexelCopyBufferLayout layout = {};
    layout.bytesPerRow = 4;
    layout.rowsPerImage = 1;
    wgpu::Extent3D extent = {1u, 1u, 1u};
    device->queue().writeTexture(dst, pixel, sizeof(pixel), layout, extent);

    dummyTextureView = dummyTexture.createView();

    // `{wgpu::Default}` initializes with `maxAnisotropy = 1`; plain `= {}`
    // leaves it at 0 which wgpu-native rejects as a validation error.
    wgpu::SamplerDescriptor sd{wgpu::Default};
    sd.label = wgpuLabel("GeoEncoderDummySampler");
    sd.addressModeU = wgpu::AddressMode::Repeat;
    sd.addressModeV = wgpu::AddressMode::Repeat;
    sd.minFilter = wgpu::FilterMode::Linear;
    sd.magFilter = wgpu::FilterMode::Linear;
    sd.maxAnisotropy = 1;
    dummySampler = dev.createSampler(sd);

    // --- Clip-mask dummy (R8Unorm, 1x1, value 0xFF = 1.0) ---
    wgpu::TextureDescriptor maskDummyDesc = {};
    maskDummyDesc.label = wgpuLabel("GeoEncoderDummyClipMask");
    maskDummyDesc.size = {1u, 1u, 1u};
    maskDummyDesc.format = wgpu::TextureFormat::R8Unorm;
    maskDummyDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    maskDummyDesc.mipLevelCount = 1;
    maskDummyDesc.sampleCount = 1;
    maskDummyDesc.dimension = wgpu::TextureDimension::_2D;
    dummyClipMaskTexture = dev.createTexture(maskDummyDesc);

    const uint8_t maskPixel[1] = {0xFF};
    wgpu::TexelCopyTextureInfo maskDst = {};
    maskDst.texture = dummyClipMaskTexture;
    wgpu::TexelCopyBufferLayout maskLayout = {};
    maskLayout.bytesPerRow = 1;
    maskLayout.rowsPerImage = 1;
    wgpu::Extent3D maskExtent = {1u, 1u, 1u};
    device->queue().writeTexture(maskDst, maskPixel, sizeof(maskPixel), maskLayout,
                                 maskExtent);

    dummyClipMaskTextureView = dummyClipMaskTexture.createView();

    // Clip mask sampler — Linear / ClampToEdge so edge coverage
    // interpolates smoothly without wrapping back to the opposite
    // side of the mask texture.
    wgpu::SamplerDescriptor maskSd{wgpu::Default};
    maskSd.label = wgpuLabel("GeoEncoderClipMaskSampler");
    maskSd.addressModeU = wgpu::AddressMode::ClampToEdge;
    maskSd.addressModeV = wgpu::AddressMode::ClampToEdge;
    maskSd.minFilter = wgpu::FilterMode::Linear;
    maskSd.magFilter = wgpu::FilterMode::Linear;
    maskSd.maxAnisotropy = 1;
    clipMaskSampler = dev.createSampler(maskSd);
  }

  /// Return the texture view that should be bound to the clip-mask
  /// slot for the next draw — the active mask if set, or the dummy
  /// otherwise. Always returns a valid view after `ensureDummyResources`.
  const wgpu::TextureView& currentClipMaskView() {
    if (activeClipMaskView) {
      return activeClipMaskView;
    }
    return dummyClipMaskTextureView;
  }

  /// Open the render pass on demand.
  void ensurePassOpen() {
    if (passOpen) {
      return;
    }
    wgpu::RenderPassColorAttachment color = {};

    // 4× MSAA color attachment with per-pass resolve. The MSAA view is
    // the draw target; WebGPU implicitly resolves into `targetView` at
    // pass end. `storeOp = Store` on the MSAA attachment preserves its
    // state for a subsequent pass (see `setLoadPreserve()` — we may
    // reopen a pass to continue drawing on top of the previous MSAA
    // contents, e.g., after a nested-layer composite).
    color.view = msaaTargetView;
    color.resolveTarget = targetView;
    color.loadOp = loadPreserve ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
    color.storeOp = wgpu::StoreOp::Store;
    color.clearValue = clearColor;

    wgpu::RenderPassDescriptor desc = {};
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color;
    desc.label = wgpuLabel("GeoEncoderPass");
    pass = commandEncoder.beginRenderPass(desc);
    // Pipelines are set per-draw — `fillPath` / `fillPathLinearGradient` /
    // `drawImage` each rebind their own pipeline before issuing a draw call.
    // Re-binding only happens when the bound pipeline differs from the next
    // one (see bindSolidPipeline / bindGradientPipeline / bindImagePipeline).
    currentPipeline = BoundPipeline::kNone;
    currentPipelineIsGradient = false;
    passOpen = true;
    // Install any deferred scissor that the renderer requested before the
    // pass was open (e.g., a `pushClip` made before the first draw of the
    // encoder). Also ensures a fresh pass re-applies the scissor if a
    // previous pass was finished and a new one opened.
    applyScissorIfPassOpen();
  }

  /// Track which pipeline is currently bound so we can emit `SetPipeline`
  /// only when it actually changes.
  enum class BoundPipeline { kNone, kSolid, kGradient, kImage };
  BoundPipeline currentPipeline = BoundPipeline::kNone;
  // Kept for backward compat with the gradient binding flag — see
  // bindGradientPipeline below.
  bool currentPipelineIsGradient = false;
  void bindSolidPipeline() {
    if (currentPipeline != BoundPipeline::kSolid) {
      pass.setPipeline(pipeline->pipeline());
      currentPipeline = BoundPipeline::kSolid;
      currentPipelineIsGradient = false;
    }
  }
  void bindGradientPipeline() {
    if (currentPipeline != BoundPipeline::kGradient) {
      pass.setPipeline(gradientPipeline->pipeline());
      currentPipeline = BoundPipeline::kGradient;
      currentPipelineIsGradient = true;
    }
  }
  void bindImagePipeline(const wgpu::RenderPipeline& imageRenderPipeline) {
    if (currentPipeline != BoundPipeline::kImage) {
      pass.setPipeline(imageRenderPipeline);
      currentPipeline = BoundPipeline::kImage;
      currentPipelineIsGradient = false;
    }
  }

  /// Build the MVP matrix from the current transform.
  /// Maps target-pixel coordinates to clip space (-1..+1 with Y down).
  void buildMvp(float* out16) const {
    // First: convert path-space → pixel space via the transform.
    // Then: pixel space → clip space.
    //   x_clip =  2 * x_pixel / width  - 1
    //   y_clip = -2 * y_pixel / height + 1   (Y flip for top-left origin)

    const double sx = 2.0 / static_cast<double>(targetWidth);
    const double sy = -2.0 / static_cast<double>(targetHeight);

    // Composed: clip = scale(sx, sy) * translate(-1, +1) * transform
    // For affine 2D transforms in row-major mat4:
    //   [ a c 0 e ]   where transform.data = [a b c d e f] (column-major 2x3)
    //   [ b d 0 f ]
    //   [ 0 0 1 0 ]
    //   [ 0 0 0 1 ]
    const double a = transform.data[0];
    const double b = transform.data[1];
    const double c = transform.data[2];
    const double d = transform.data[3];
    const double e = transform.data[4];
    const double f = transform.data[5];

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
};

GeoEncoder::GeoEncoder(GeodeDevice& device, const GeodePipeline& fillPipeline,
                       const GeodeGradientPipeline& gradientPipeline,
                       const GeodeImagePipeline& imagePipeline,
                       const wgpu::Texture& msaaTarget, const wgpu::Texture& resolveTarget)
    : impl_(std::make_unique<Impl>()) {
  impl_->device = &device;
  impl_->pipeline = &fillPipeline;
  impl_->gradientPipeline = &gradientPipeline;
  impl_->imagePipeline = &imagePipeline;
  impl_->msaaTarget = msaaTarget;
  impl_->msaaTargetView = msaaTarget.createView();
  impl_->target = resolveTarget;
  impl_->targetView = resolveTarget.createView();
  impl_->targetWidth = resolveTarget.getWidth();
  impl_->targetHeight = resolveTarget.getHeight();

  wgpu::CommandEncoderDescriptor desc = {};
  desc.label = wgpuLabel("GeoEncoder");
  impl_->commandEncoder = device.device().createCommandEncoder(desc);
}

GeoEncoder::~GeoEncoder() = default;
GeoEncoder::GeoEncoder(GeoEncoder&&) noexcept = default;
GeoEncoder& GeoEncoder::operator=(GeoEncoder&&) noexcept = default;

void GeoEncoder::clear(const css::RGBA& color) {
  // If the pass is already open, the clear has effectively been baked in;
  // recording a "clear" mid-pass is unsupported in this MVP. Document and
  // ignore — the right approach for mid-pass clears is to draw a fullscreen
  // quad, which we can add later if needed.
  if (impl_->passOpen) {
    return;
  }
  // Premultiply the clear color by alpha to match the pipeline's
  // premultiplied blend state. `fillPath` premultiplies its paint color the
  // same way, and the read-back in `RendererGeode::takeSnapshot` assumes the
  // texture contents are premultiplied throughout. Clearing with a straight-
  // alpha value would break that invariant for any semi-transparent clear.
  const double alpha = color.a / 255.0;
  impl_->clearColor.r = (color.r / 255.0) * alpha;
  impl_->clearColor.g = (color.g / 255.0) * alpha;
  impl_->clearColor.b = (color.b / 255.0) * alpha;
  impl_->clearColor.a = alpha;
  impl_->hasExplicitClear = true;
}

void GeoEncoder::setTransform(const Transform2d& transform) {
  impl_->transform = transform;
}

void GeoEncoder::setScissorRect(int32_t x, int32_t y, int32_t w, int32_t h) {
  // Clamp negative / overflowing values at the u32 boundary so WebGPU's
  // strict validation never sees an out-of-range value. A scissor with
  // zero area is valid (clips everything).
  const int32_t clampedX = std::max(0, x);
  const int32_t clampedY = std::max(0, y);
  const int32_t clampedW = std::max(0, w - (clampedX - x));
  const int32_t clampedH = std::max(0, h - (clampedY - y));
  impl_->scissorActive = true;
  impl_->scissorX = static_cast<uint32_t>(clampedX);
  impl_->scissorY = static_cast<uint32_t>(clampedY);
  impl_->scissorW = static_cast<uint32_t>(clampedW);
  impl_->scissorH = static_cast<uint32_t>(clampedH);
  impl_->applyScissorIfPassOpen();
}

void GeoEncoder::clearScissorRect() {
  impl_->scissorActive = false;
  impl_->applyScissorIfPassOpen();
}

void GeoEncoder::setClipPolygon(const Vector2d corners[4]) {
  // Compute the inward-facing half-plane for each of the 4 edges of the
  // convex polygon. Edge i runs from `corners[i]` to `corners[(i+1)%4]`,
  // with direction `d = corners[(i+1)%4] - corners[i]`. The inward
  // normal is `n = (-d.y, d.x)` when the winding is counter-clockwise
  // in screen space (which in SVG's y-down coord system is "clockwise
  // when viewed on-screen"). We DETECT the winding by computing the
  // signed area of the polygon; if the area is negative we flip the
  // normals so the half-plane equations always point *inside*.
  //
  // Each plane is stored as (a, b, c, pad) where `a*x + b*y + c >= 0`
  // marks the inside half-plane. `c = -(a*corners[i].x + b*corners[i].y)`
  // offsets the plane to pass through the edge start.

  // Signed area (Shoelace formula / 2). Positive → CCW in standard math
  // (y-up) but in SVG (y-down) that maps to a CW visual winding.
  double signedArea = 0.0;
  for (size_t i = 0; i < 4; ++i) {
    const Vector2d& p0 = corners[i];
    const Vector2d& p1 = corners[(i + 1) % 4];
    signedArea += (p0.x * p1.y) - (p1.x * p0.y);
  }
  const double windingSign = signedArea >= 0.0 ? 1.0 : -1.0;

  for (size_t i = 0; i < 4; ++i) {
    const Vector2d& p0 = corners[i];
    const Vector2d& p1 = corners[(i + 1) % 4];
    const double dx = p1.x - p0.x;
    const double dy = p1.y - p0.y;
    // Inward normal: rotate (dx, dy) by +90° (= (-dy, dx)) and flip if
    // the overall polygon winding is negative.
    double nx = -dy * windingSign;
    double ny = dx * windingSign;
    // Normalise so the half-plane value is in viewport-pixel units
    // (makes the per-sample test resolution-independent).
    const double len = std::sqrt(nx * nx + ny * ny);
    if (len > 1e-12) {
      nx /= len;
      ny /= len;
    }
    const double c = -(nx * p0.x + ny * p0.y);
    impl_->clipPolygonPlanes[i * 4 + 0] = static_cast<float>(nx);
    impl_->clipPolygonPlanes[i * 4 + 1] = static_cast<float>(ny);
    impl_->clipPolygonPlanes[i * 4 + 2] = static_cast<float>(c);
    impl_->clipPolygonPlanes[i * 4 + 3] = 0.0f;
  }
  impl_->clipPolygonActive = true;
}

void GeoEncoder::clearClipPolygon() {
  impl_->clipPolygonActive = false;
  for (size_t i = 0; i < 16; ++i) {
    impl_->clipPolygonPlanes[i] = 0.0f;
  }
}

// ============================================================================
// Phase 3b: clip mask pass
// ============================================================================

void GeoEncoder::beginMaskPass(const wgpu::Texture& msaaMask,
                               const wgpu::Texture& resolveMask) {
  if (!msaaMask || !resolveMask) {
    return;
  }

  // Close the current main render pass so the new mask pass can open
  // against the mask texture. A subsequent main draw will lazily
  // reopen the main pass with `LoadOp::Load` via `setLoadPreserve()`.
  // Only flip `loadPreserve` when the main pass was *actually* open:
  // if no main draw has landed yet the initial clear still needs to
  // run on the next open.
  const bool mainPassWasOpen = impl_->passOpen;
  if (mainPassWasOpen) {
    impl_->pass.end();
    impl_->passOpen = false;
    impl_->loadPreserve = true;
  }

  // Lazily build the mask pipeline on first use.
  if (!impl_->maskPipelineOwned) {
    impl_->maskPipelineOwned =
        std::make_unique<GeodeMaskPipeline>(impl_->device->device(),
                                            impl_->device->useAlphaCoverageAA());
  }

  impl_->maskPassSavedTransform = impl_->transform;

  wgpu::TextureView msaaView = msaaMask.createView();
  wgpu::TextureView resolveView = resolveMask.createView();

  wgpu::RenderPassColorAttachment color = {};
  color.view = msaaView;
  color.resolveTarget = resolveView;
  color.loadOp = wgpu::LoadOp::Clear;
  color.storeOp = wgpu::StoreOp::Store;
  color.clearValue = {0.0, 0.0, 0.0, 0.0};

  wgpu::RenderPassDescriptor desc = {};
  desc.colorAttachmentCount = 1;
  desc.colorAttachments = &color;
  desc.label = wgpuLabel("GeoEncoderMaskPass");
  impl_->maskPass = impl_->commandEncoder.beginRenderPass(desc);
  impl_->maskPass.setPipeline(impl_->maskPipelineOwned->pipeline());
  // Full-target scissor so clip-path fills aren't clipped by any
  // outer scissor still cached in the encoder state.
  impl_->maskPass.setScissorRect(0, 0, impl_->targetWidth, impl_->targetHeight);
  impl_->maskPassOpen = true;
}

void GeoEncoder::fillPathIntoMask(const Path& path, FillRule rule) {
  if (!impl_->maskPassOpen) {
    return;
  }
  // The mask pipeline now samples a nested clip mask via bindings
  // 3/4, so it needs the dummy mask texture + sampler bound when
  // no deeper layer is active. `ensureDummyResources` is idempotent
  // and normally runs during the main draw path, but `beginMaskPass`
  // can be called BEFORE any main draw on a fresh encoder.
  impl_->ensureDummyResources();
  EncodedPath encoded = GeodePathEncoder::encode(path, rule);
  if (encoded.empty()) {
    return;
  }

  const wgpu::Device& dev = impl_->device->device();
  const wgpu::Queue& queue = impl_->device->queue();

  const uint64_t vbSize = roundUp4(encoded.vertices.size() * sizeof(EncodedPath::Vertex));
  wgpu::BufferDescriptor vbDesc = {};
  vbDesc.label = wgpuLabel("GeodeMaskVB");
  vbDesc.size = vbSize;
  vbDesc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer vb = dev.createBuffer(vbDesc);
  queue.writeBuffer(vb, 0, encoded.vertices.data(),
                    encoded.vertices.size() * sizeof(EncodedPath::Vertex));

  const uint64_t bandsSize = roundUp4(encoded.bands.size() * sizeof(EncodedPath::Band));
  wgpu::BufferDescriptor bandsDesc = {};
  bandsDesc.label = wgpuLabel("GeodeMaskBandsSSBO");
  bandsDesc.size = bandsSize;
  bandsDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer bandsBuf = dev.createBuffer(bandsDesc);
  queue.writeBuffer(bandsBuf, 0, encoded.bands.data(),
                    encoded.bands.size() * sizeof(EncodedPath::Band));

  const uint64_t curveFloats = encoded.curves.size() * 6u;
  const uint64_t curvesSize = roundUp4(curveFloats * sizeof(float));
  wgpu::BufferDescriptor curvesDesc = {};
  curvesDesc.label = wgpuLabel("GeodeMaskCurvesSSBO");
  curvesDesc.size = curvesSize;
  curvesDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer curvesBuf = dev.createBuffer(curvesDesc);
  queue.writeBuffer(curvesBuf, 0, encoded.curves.data(),
                    encoded.curves.size() * sizeof(EncodedPath::Curve));

  // Mask uniforms — mvp, viewport, fillRule, hasClipMask. The last
  // field gates whether the fragment shader samples the nested clip
  // mask at binding 3 (used for nested `<clipPath>` references —
  // each outer-layer shape is intersected with the deeper layer's
  // already-rendered union).
  struct alignas(16) MaskUniforms {
    float mvp[16];        //  0 ..  64
    float viewport[2];    // 64 ..  72
    uint32_t fillRule;    // 72 ..  76
    uint32_t hasClipMask; // 76 ..  80
  };
  static_assert(sizeof(MaskUniforms) == 80, "MaskUniforms layout mismatch");

  MaskUniforms u = {};
  impl_->buildMvp(u.mvp);
  u.viewport[0] = static_cast<float>(impl_->targetWidth);
  u.viewport[1] = static_cast<float>(impl_->targetHeight);
  u.fillRule = (rule == FillRule::EvenOdd) ? 1u : 0u;
  u.hasClipMask = impl_->activeClipMaskView ? 1u : 0u;

  wgpu::BufferDescriptor uniDesc = {};
  uniDesc.label = wgpuLabel("GeodeMaskUniforms");
  uniDesc.size = sizeof(MaskUniforms);
  uniDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer uniBuf = dev.createBuffer(uniDesc);
  queue.writeBuffer(uniBuf, 0, &u, sizeof(MaskUniforms));

  wgpu::BindGroupEntry entries[5] = {};
  entries[0].binding = 0;
  entries[0].buffer = uniBuf;
  entries[0].size = sizeof(MaskUniforms);
  entries[1].binding = 1;
  entries[1].buffer = bandsBuf;
  entries[1].size = bandsSize;
  entries[2].binding = 2;
  entries[2].buffer = curvesBuf;
  entries[2].size = curvesSize;
  entries[3].binding = 3;
  entries[3].textureView = impl_->currentClipMaskView();
  entries[4].binding = 4;
  entries[4].sampler = impl_->clipMaskSampler;

  wgpu::BindGroupDescriptor bgDesc = {};
  bgDesc.label = wgpuLabel("GeodeMaskBindGroup");
  bgDesc.layout = impl_->maskPipelineOwned->bindGroupLayout();
  bgDesc.entryCount = 5;
  bgDesc.entries = entries;
  wgpu::BindGroup bindGroup = dev.createBindGroup(bgDesc);

  impl_->maskPass.setVertexBuffer(0, vb, 0, vbSize);
  impl_->maskPass.setBindGroup(0, bindGroup, 0, nullptr);
  impl_->maskPass.draw(static_cast<uint32_t>(encoded.vertices.size()), 1, 0, 0);
}

void GeoEncoder::endMaskPass() {
  if (!impl_->maskPassOpen) {
    return;
  }
  impl_->maskPass.end();
  impl_->maskPassOpen = false;
  // Rebind pipeline tracker — the main pass will need to re-select a
  // pipeline on its next draw.
  impl_->currentPipeline = Impl::BoundPipeline::kNone;
  impl_->currentPipelineIsGradient = false;
  // Restore encoder transform in case the caller trampled it with a
  // mask-local transform.
  impl_->transform = impl_->maskPassSavedTransform;
}

void GeoEncoder::setClipMask(const wgpu::TextureView& maskView) {
  impl_->activeClipMaskView = maskView;
}

void GeoEncoder::clearClipMask() {
  impl_->activeClipMaskView = wgpu::TextureView{};
}

void GeoEncoder::setLoadPreserve() {
  // No-op if a pass is already open — loadOp is a pass-construction
  // parameter and can't be changed mid-pass. The RendererGeode caller
  // always invokes this right after constructing a fresh encoder, so the
  // pass isn't open yet in practice.
  if (impl_->passOpen) {
    return;
  }
  impl_->loadPreserve = true;
}

/// Parameters for the shared `submitFillDraw` helper. Solid-color and
/// pattern-tile fills share the same encode → upload → draw pipeline; only
/// the uniform contents and bound texture differ.
struct GeoEncoder::FillDrawArgs {
  const Path* path;
  FillRule rule;

  // Paint mode selector. 0 = solid, 1 = pattern.
  uint32_t paintMode;

  // Solid-fill-only fields (ignored when paintMode == 1).
  float solidColor[4];  // Premultiplied.

  // Pattern-fill-only fields (ignored when paintMode == 0).
  wgpu::TextureView patternView;
  wgpu::Sampler patternSampler;
  Transform2d patternFromPath;
  Vector2d tileSize;
  float patternOpacity;
};

void GeoEncoder::fillPath(const Path& path, const css::RGBA& color, FillRule rule) {
  FillDrawArgs args = {};
  args.path = &path;
  args.rule = rule;
  args.paintMode = 0u;
  const float alpha = color.a / 255.0f;
  args.solidColor[0] = (color.r / 255.0f) * alpha;
  args.solidColor[1] = (color.g / 255.0f) * alpha;
  args.solidColor[2] = (color.b / 255.0f) * alpha;
  args.solidColor[3] = alpha;
  args.patternOpacity = 1.0f;

  // Use dummy texture/sampler in solid mode so the bind group is always
  // complete. These are lazily initialised.
  impl_->ensureDummyResources();
  args.patternView = impl_->dummyTextureView;
  args.patternSampler = impl_->dummySampler;
  args.tileSize = Vector2d(1.0, 1.0);
  args.patternFromPath = Transform2d();

  submitFillDraw(args);
}

void GeoEncoder::fillPathPattern(const Path& path, FillRule rule,
                                 const PatternPaint& paint) {
  if (!paint.tile || paint.tileSize.x <= 0.0 || paint.tileSize.y <= 0.0) {
    return;
  }

  // Build a sampler for the tile. We use linear filtering with Repeat
  // wrap mode; the shader also performs explicit modulo-style wrapping
  // via `fract()` so texture sampling never steps outside [0,1] UVs, but
  // Repeat is still the right conceptual wrap mode for any implicit
  // derivative / mip work WebGPU might do on the sampler.
  wgpu::SamplerDescriptor sd{wgpu::Default};
  sd.label = wgpuLabel("GeoEncoderPatternSampler");
  sd.addressModeU = wgpu::AddressMode::Repeat;
  sd.addressModeV = wgpu::AddressMode::Repeat;
  sd.minFilter = wgpu::FilterMode::Linear;
  sd.magFilter = wgpu::FilterMode::Linear;
  sd.maxAnisotropy = 1;
  wgpu::Sampler sampler = impl_->device->device().createSampler(sd);

  FillDrawArgs args = {};
  args.path = &path;
  args.rule = rule;
  args.paintMode = 1u;
  args.patternView = paint.tile.createView();
  args.patternSampler = sampler;
  args.patternFromPath = paint.patternFromPath;
  args.tileSize = paint.tileSize;
  args.patternOpacity = static_cast<float>(paint.opacity);

  submitFillDraw(args);
}

void GeoEncoder::submitFillDraw(const FillDrawArgs& args) {
  // Always prepare the clip-mask dummy texture + sampler; the bind
  // group layout requires a valid texture view at binding 5 and a
  // valid sampler at binding 6 regardless of whether a clip is
  // active.
  impl_->ensureDummyResources();
  impl_->ensurePassOpen();
  impl_->bindSolidPipeline();

  // The pass shares the Slug fill pipeline and the image-blit pipeline;
  // always set this path's pipeline before issuing the draw so a preceding
  // `drawImage` (or any future pipeline-switching helper) doesn't leak state.
  impl_->pass.setPipeline(impl_->pipeline->pipeline());

  // 1. CPU encode the path into Slug band data.
  EncodedPath encoded = GeodePathEncoder::encode(*args.path, args.rule);
  if (encoded.empty()) {
    return;  // Nothing to draw.
  }

  const wgpu::Device& dev = impl_->device->device();
  const wgpu::Queue& queue = impl_->device->queue();

  // 2. Allocate and upload GPU buffers.
  const uint64_t vbSize =
      roundUp4(encoded.vertices.size() * sizeof(EncodedPath::Vertex));
  wgpu::BufferDescriptor vbDesc = {};
  vbDesc.label = wgpuLabel("GeodeVB");
  vbDesc.size = vbSize;
  vbDesc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer vb = dev.createBuffer(vbDesc);
  queue.writeBuffer(vb, 0, encoded.vertices.data(),
                    encoded.vertices.size() * sizeof(EncodedPath::Vertex));

  const uint64_t bandsSize = roundUp4(encoded.bands.size() * sizeof(EncodedPath::Band));
  wgpu::BufferDescriptor bandsDesc = {};
  bandsDesc.label = wgpuLabel("GeodeBandsSSBO");
  bandsDesc.size = bandsSize;
  bandsDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer bandsBuf = dev.createBuffer(bandsDesc);
  queue.writeBuffer(bandsBuf, 0, encoded.bands.data(),
                    encoded.bands.size() * sizeof(EncodedPath::Band));

  const uint64_t curveFloats = encoded.curves.size() * 6u;
  const uint64_t curvesSize = roundUp4(curveFloats * sizeof(float));
  wgpu::BufferDescriptor curvesDesc = {};
  curvesDesc.label = wgpuLabel("GeodeCurvesSSBO");
  curvesDesc.size = curvesSize;
  curvesDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer curvesBuf = dev.createBuffer(curvesDesc);
  queue.writeBuffer(curvesBuf, 0, encoded.curves.data(),
                    encoded.curves.size() * sizeof(EncodedPath::Curve));

  // Uniform buffer.
  Uniforms u = {};
  impl_->buildMvp(u.mvp);
  affineToMat4(args.patternFromPath, u.patternFromPath);
  u.viewport[0] = static_cast<float>(impl_->targetWidth);
  u.viewport[1] = static_cast<float>(impl_->targetHeight);
  u.tileSize[0] = static_cast<float>(args.tileSize.x);
  u.tileSize[1] = static_cast<float>(args.tileSize.y);
  u.color[0] = args.solidColor[0];
  u.color[1] = args.solidColor[1];
  u.color[2] = args.solidColor[2];
  u.color[3] = args.solidColor[3];
  u.fillRule = (args.rule == FillRule::EvenOdd) ? 1u : 0u;
  u.paintMode = args.paintMode;
  u.patternOpacity = args.patternOpacity;
  impl_->writeClipPolygonUniforms(u.hasClipPolygon, u.clipPolygonPlanes);
  u.hasClipMask = impl_->activeClipMaskView ? 1u : 0u;

  wgpu::BufferDescriptor uniDesc = {};
  uniDesc.label = wgpuLabel("GeodeUniforms");
  uniDesc.size = sizeof(Uniforms);
  uniDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer uniBuf = dev.createBuffer(uniDesc);
  queue.writeBuffer(uniBuf, 0, &u, sizeof(Uniforms));

  // 3. Bind group — seven entries: uniforms, bands SSBO, curves SSBO,
  // pattern texture, pattern sampler, clip-mask texture, clip-mask
  // sampler. Solid-fill draws bind dummies for the pattern slot and
  // the clip-mask slot binds either the dummy (hasClipMask == 0) or
  // the active mask from `setClipMask`.
  wgpu::BindGroupEntry entries[7] = {};
  entries[0].binding = 0;
  entries[0].buffer = uniBuf;
  entries[0].size = sizeof(Uniforms);
  entries[1].binding = 1;
  entries[1].buffer = bandsBuf;
  entries[1].size = bandsSize;
  entries[2].binding = 2;
  entries[2].buffer = curvesBuf;
  entries[2].size = curvesSize;
  entries[3].binding = 3;
  entries[3].textureView = args.patternView;
  entries[4].binding = 4;
  entries[4].sampler = args.patternSampler;
  entries[5].binding = 5;
  entries[5].textureView = impl_->currentClipMaskView();
  entries[6].binding = 6;
  entries[6].sampler = impl_->clipMaskSampler;

  wgpu::BindGroupDescriptor bgDesc = {};
  bgDesc.label = wgpuLabel("GeodeBindGroup");
  bgDesc.layout = impl_->pipeline->bindGroupLayout();
  bgDesc.entryCount = 7;
  bgDesc.entries = entries;
  wgpu::BindGroup bindGroup = dev.createBindGroup(bgDesc);

  // 4. Record the draw call.
  impl_->pass.setVertexBuffer(0, vb, 0, vbSize);
  impl_->pass.setBindGroup(0, bindGroup, 0, nullptr);
  impl_->pass.draw(static_cast<uint32_t>(encoded.vertices.size()), 1, 0, 0);
}

namespace {

/// Populate the non-kind-specific fields of a `GradientUniforms` struct from
/// the shared parameters (transform, spread mode, stops) passed to any
/// gradient-fill call. The caller fills in `gradientKind` and any
/// kind-specific fields (start/end for linear, center/focal/radii for
/// radial) afterwards.
template <typename StopT>
void populateSharedGradientUniforms(GradientUniforms& u, const Transform2d& gradientFromPath,
                                    uint32_t spreadMode, std::span<const StopT> stops,
                                    FillRule rule) {
  u.fillRule = (rule == FillRule::EvenOdd) ? 1u : 0u;
  u.spreadMode = spreadMode;

  // Pack `gradientFromPath` as two row-vectors:
  //   row0 = (a, c, e, 0) → gx = a*px + c*py + e
  //   row1 = (b, d, f, 0) → gy = b*px + d*py + f
  // (Transform2d::data is column-major 2x3: [a, b, c, d, e, f].)
  u.row0[0] = static_cast<float>(gradientFromPath.data[0]);
  u.row0[1] = static_cast<float>(gradientFromPath.data[2]);
  u.row0[2] = static_cast<float>(gradientFromPath.data[4]);
  u.row0[3] = 0.0f;
  u.row1[0] = static_cast<float>(gradientFromPath.data[1]);
  u.row1[1] = static_cast<float>(gradientFromPath.data[3]);
  u.row1[2] = static_cast<float>(gradientFromPath.data[5]);
  u.row1[3] = 0.0f;

  // Upload stops in STRAIGHT (unpremultiplied) alpha. The shader's
  // `sample_stops` linearly interpolates between these values and then the
  // fragment stage premultiplies at output before the premultiplied-alpha
  // blend pipeline composites onto the framebuffer. This matches tiny-skia /
  // Skia gradient behavior — e.g. `a-stop-opacity-001` transitions from
  // white to black@0.2 and expects straight-space interpolation, not
  // premultiplied-space mix. Clamp the stop count to the shader's hard cap;
  // overflow was warned about at the caller.
  const uint32_t stopCount =
      std::min<uint32_t>(kMaxGradientStops, static_cast<uint32_t>(stops.size()));
  u.stopCount = stopCount;
  for (uint32_t i = 0; i < stopCount; ++i) {
    const StopT& s = stops[i];
    u.stopColors[i * 4 + 0] = s.rgba[0];
    u.stopColors[i * 4 + 1] = s.rgba[1];
    u.stopColors[i * 4 + 2] = s.rgba[2];
    u.stopColors[i * 4 + 3] = s.rgba[3];
    // Packed 4-per-vec4: stop i lives in stopOffsets[i/4].(x|y|z|w).
    u.stopOffsets[i] = s.offset;
  }
}

}  // namespace

void GeoEncoder::fillPathLinearGradient(const Path& path, const LinearGradientParams& params,
                                        FillRule rule) {
  if (params.stops.empty()) {
    return;
  }

  // The gradient bind group now has a clip-mask texture binding (see
  // Phase 3b); we need a dummy bound when no clip is active.
  impl_->ensureDummyResources();
  impl_->ensurePassOpen();
  impl_->bindGradientPipeline();

  // 1. CPU encode the path into Slug band data (same as fillPath).
  EncodedPath encoded = GeodePathEncoder::encode(path, rule);
  if (encoded.empty()) {
    return;
  }

  const wgpu::Device& dev = impl_->device->device();
  const wgpu::Queue& queue = impl_->device->queue();

  // 2. Allocate and upload GPU buffers.
  const uint64_t vbSize =
      roundUp4(encoded.vertices.size() * sizeof(EncodedPath::Vertex));
  wgpu::BufferDescriptor vbDesc = {};
  vbDesc.label = wgpuLabel("GeodeGradientVB");
  vbDesc.size = vbSize;
  vbDesc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer vb = dev.createBuffer(vbDesc);
  queue.writeBuffer(vb, 0, encoded.vertices.data(),
                    encoded.vertices.size() * sizeof(EncodedPath::Vertex));

  const uint64_t bandsSize = roundUp4(encoded.bands.size() * sizeof(EncodedPath::Band));
  wgpu::BufferDescriptor bandsDesc = {};
  bandsDesc.label = wgpuLabel("GeodeGradientBandsSSBO");
  bandsDesc.size = bandsSize;
  bandsDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer bandsBuf = dev.createBuffer(bandsDesc);
  queue.writeBuffer(bandsBuf, 0, encoded.bands.data(),
                    encoded.bands.size() * sizeof(EncodedPath::Band));

  const uint64_t curveFloats = encoded.curves.size() * 6u;
  const uint64_t curvesSize = roundUp4(curveFloats * sizeof(float));
  wgpu::BufferDescriptor curvesDesc = {};
  curvesDesc.label = wgpuLabel("GeodeGradientCurvesSSBO");
  curvesDesc.size = curvesSize;
  curvesDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer curvesBuf = dev.createBuffer(curvesDesc);
  queue.writeBuffer(curvesBuf, 0, encoded.curves.data(),
                    encoded.curves.size() * sizeof(EncodedPath::Curve));

  // 3. Build gradient uniforms.
  GradientUniforms u = {};
  impl_->buildMvp(u.mvp);
  u.viewport[0] = static_cast<float>(impl_->targetWidth);
  u.viewport[1] = static_cast<float>(impl_->targetHeight);
  populateSharedGradientUniforms<LinearGradientParams::Stop>(
      u, params.gradientFromPath, params.spreadMode, params.stops, rule);
  u.gradientKind = kGradientKindLinear;
  impl_->writeClipPolygonUniforms(u.hasClipPolygon, u.clipPolygonPlanes);
  u.hasClipMask = impl_->activeClipMaskView ? 1u : 0u;

  u.startGrad[0] = static_cast<float>(params.startGrad.x);
  u.startGrad[1] = static_cast<float>(params.startGrad.y);
  u.endGrad[0] = static_cast<float>(params.endGrad.x);
  u.endGrad[1] = static_cast<float>(params.endGrad.y);

  wgpu::BufferDescriptor uniDesc = {};
  uniDesc.label = wgpuLabel("GeodeGradientUniforms");
  uniDesc.size = sizeof(GradientUniforms);
  uniDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer uniBuf = dev.createBuffer(uniDesc);
  queue.writeBuffer(uniBuf, 0, &u, sizeof(GradientUniforms));

  // 4. Bind group — five entries: uniforms, bands SSBO, curves SSBO,
  // clip-mask texture, clip-mask sampler.
  wgpu::BindGroupEntry entries[5] = {};
  entries[0].binding = 0;
  entries[0].buffer = uniBuf;
  entries[0].size = sizeof(GradientUniforms);
  entries[1].binding = 1;
  entries[1].buffer = bandsBuf;
  entries[1].size = bandsSize;
  entries[2].binding = 2;
  entries[2].buffer = curvesBuf;
  entries[2].size = curvesSize;
  entries[3].binding = 3;
  entries[3].textureView = impl_->currentClipMaskView();
  entries[4].binding = 4;
  entries[4].sampler = impl_->clipMaskSampler;

  wgpu::BindGroupDescriptor bgDesc = {};
  bgDesc.label = wgpuLabel("GeodeGradientBindGroup");
  bgDesc.layout = impl_->gradientPipeline->bindGroupLayout();
  bgDesc.entryCount = 5;
  bgDesc.entries = entries;
  wgpu::BindGroup bindGroup = dev.createBindGroup(bgDesc);

  impl_->pass.setVertexBuffer(0, vb, 0, vbSize);
  impl_->pass.setBindGroup(0, bindGroup, 0, nullptr);
  impl_->pass.draw(static_cast<uint32_t>(encoded.vertices.size()), 1, 0, 0);
}

void GeoEncoder::fillPathRadialGradient(const Path& path, const RadialGradientParams& params,
                                        FillRule rule) {
  if (params.stops.empty()) {
    return;
  }
  // Degenerate radius: nothing to draw meaningfully — match tiny-skia's
  // early return. A radius of zero collapses the quadratic, and the caller
  // should have dropped the draw anyway.
  if (params.radius <= 0.0) {
    return;
  }

  // Dummy texture for the clip-mask slot, see fillPathLinearGradient.
  impl_->ensureDummyResources();
  impl_->ensurePassOpen();
  impl_->bindGradientPipeline();

  EncodedPath encoded = GeodePathEncoder::encode(path, rule);
  if (encoded.empty()) {
    return;
  }

  const wgpu::Device& dev = impl_->device->device();
  const wgpu::Queue& queue = impl_->device->queue();

  const uint64_t vbSize =
      roundUp4(encoded.vertices.size() * sizeof(EncodedPath::Vertex));
  wgpu::BufferDescriptor vbDesc = {};
  vbDesc.label = wgpuLabel("GeodeRadialGradientVB");
  vbDesc.size = vbSize;
  vbDesc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer vb = dev.createBuffer(vbDesc);
  queue.writeBuffer(vb, 0, encoded.vertices.data(),
                    encoded.vertices.size() * sizeof(EncodedPath::Vertex));

  const uint64_t bandsSize = roundUp4(encoded.bands.size() * sizeof(EncodedPath::Band));
  wgpu::BufferDescriptor bandsDesc = {};
  bandsDesc.label = wgpuLabel("GeodeRadialGradientBandsSSBO");
  bandsDesc.size = bandsSize;
  bandsDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer bandsBuf = dev.createBuffer(bandsDesc);
  queue.writeBuffer(bandsBuf, 0, encoded.bands.data(),
                    encoded.bands.size() * sizeof(EncodedPath::Band));

  const uint64_t curveFloats = encoded.curves.size() * 6u;
  const uint64_t curvesSize = roundUp4(curveFloats * sizeof(float));
  wgpu::BufferDescriptor curvesDesc = {};
  curvesDesc.label = wgpuLabel("GeodeRadialGradientCurvesSSBO");
  curvesDesc.size = curvesSize;
  curvesDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer curvesBuf = dev.createBuffer(curvesDesc);
  queue.writeBuffer(curvesBuf, 0, encoded.curves.data(),
                    encoded.curves.size() * sizeof(EncodedPath::Curve));

  GradientUniforms u = {};
  impl_->buildMvp(u.mvp);
  u.viewport[0] = static_cast<float>(impl_->targetWidth);
  u.viewport[1] = static_cast<float>(impl_->targetHeight);
  populateSharedGradientUniforms<RadialGradientParams::Stop>(
      u, params.gradientFromPath, params.spreadMode, params.stops, rule);
  u.gradientKind = kGradientKindRadial;
  impl_->writeClipPolygonUniforms(u.hasClipPolygon, u.clipPolygonPlanes);
  u.hasClipMask = impl_->activeClipMaskView ? 1u : 0u;

  u.radialCenter[0] = static_cast<float>(params.center.x);
  u.radialCenter[1] = static_cast<float>(params.center.y);
  u.radialFocal[0] = static_cast<float>(params.focalCenter.x);
  u.radialFocal[1] = static_cast<float>(params.focalCenter.y);
  u.radialRadius = static_cast<float>(params.radius);
  u.radialFocalRadius = static_cast<float>(params.focalRadius);

  wgpu::BufferDescriptor uniDesc = {};
  uniDesc.label = wgpuLabel("GeodeRadialGradientUniforms");
  uniDesc.size = sizeof(GradientUniforms);
  uniDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer uniBuf = dev.createBuffer(uniDesc);
  queue.writeBuffer(uniBuf, 0, &u, sizeof(GradientUniforms));

  wgpu::BindGroupEntry entries[5] = {};
  entries[0].binding = 0;
  entries[0].buffer = uniBuf;
  entries[0].size = sizeof(GradientUniforms);
  entries[1].binding = 1;
  entries[1].buffer = bandsBuf;
  entries[1].size = bandsSize;
  entries[2].binding = 2;
  entries[2].buffer = curvesBuf;
  entries[2].size = curvesSize;
  entries[3].binding = 3;
  entries[3].textureView = impl_->currentClipMaskView();
  entries[4].binding = 4;
  entries[4].sampler = impl_->clipMaskSampler;

  wgpu::BindGroupDescriptor bgDesc = {};
  bgDesc.label = wgpuLabel("GeodeRadialGradientBindGroup");
  bgDesc.layout = impl_->gradientPipeline->bindGroupLayout();
  bgDesc.entryCount = 5;
  bgDesc.entries = entries;
  wgpu::BindGroup bindGroup = dev.createBindGroup(bgDesc);

  impl_->pass.setVertexBuffer(0, vb, 0, vbSize);
  impl_->pass.setBindGroup(0, bindGroup, 0, nullptr);
  impl_->pass.draw(static_cast<uint32_t>(encoded.vertices.size()), 1, 0, 0);
}

void GeoEncoder::blitFullTarget(const wgpu::Texture& src, double opacity) {
  if (!src) {
    return;
  }
  impl_->ensurePassOpen();
  impl_->bindImagePipeline(impl_->imagePipeline->pipeline());

  // Identity MVP: map target-pixel coords (0..W, 0..H) directly to clip
  // space (-1..+1 with Y flipped). This is the same math
  // `Impl::buildMvp` does with `transform == identity`.
  const double sx = 2.0 / static_cast<double>(impl_->targetWidth);
  const double sy = -2.0 / static_cast<double>(impl_->targetHeight);
  float mvp[16] = {0};
  mvp[0] = static_cast<float>(sx);
  mvp[5] = static_cast<float>(sy);
  mvp[10] = 1.0f;
  mvp[12] = -1.0f;
  mvp[13] = 1.0f;
  mvp[15] = 1.0f;

  GeodeTextureEncoder::QuadParams qp;
  qp.destRect = Box2d(Vector2d(0.0, 0.0),
                      Vector2d(static_cast<double>(impl_->targetWidth),
                               static_cast<double>(impl_->targetHeight)));
  qp.srcRect = Box2d({0.0, 0.0}, {1.0, 1.0});
  qp.opacity = opacity;
  qp.filter = GeodeTextureEncoder::Filter::Linear;
  // Layer textures are offscreen render targets produced by the Geode
  // premultiplied source-over pipeline, so their storage is already in
  // premultiplied-alpha form. The shader needs to skip its default
  // straight→premult conversion, otherwise nested `pushIsolatedLayer`
  // calls double-darken the RGB channel (e.g.
  // structure/use/opacity-inheritance where a use opacity=0.5 wraps a
  // rect opacity=0.5 and should composite to 0.25).
  qp.sourceIsPremultiplied = true;

  GeodeTextureEncoder::drawTexturedQuad(*impl_->device, *impl_->imagePipeline, impl_->pass, src,
                                        mvp, impl_->targetWidth, impl_->targetHeight, qp);
}

void GeoEncoder::blitFullTargetMasked(const wgpu::Texture& content, const wgpu::Texture& mask,
                                      const std::optional<Box2d>& maskBounds) {
  if (!content || !mask) {
    return;
  }
  impl_->ensurePassOpen();
  impl_->bindImagePipeline(impl_->imagePipeline->pipeline());

  // Identity MVP for target-pixel → clip space, same as blitFullTarget.
  const double sx = 2.0 / static_cast<double>(impl_->targetWidth);
  const double sy = -2.0 / static_cast<double>(impl_->targetHeight);
  float mvp[16] = {0};
  mvp[0] = static_cast<float>(sx);
  mvp[5] = static_cast<float>(sy);
  mvp[10] = 1.0f;
  mvp[12] = -1.0f;
  mvp[13] = 1.0f;
  mvp[15] = 1.0f;

  GeodeTextureEncoder::QuadParams qp;
  qp.destRect = Box2d(Vector2d(0.0, 0.0),
                      Vector2d(static_cast<double>(impl_->targetWidth),
                               static_cast<double>(impl_->targetHeight)));
  qp.srcRect = Box2d({0.0, 0.0}, {1.0, 1.0});
  qp.opacity = 1.0;
  qp.filter = GeodeTextureEncoder::Filter::Linear;
  // Both content and mask are offscreen render targets produced by
  // Geode's premultiplied source-over pipeline, so they're already
  // in premultiplied alpha.
  qp.sourceIsPremultiplied = true;
  qp.maskTexture = mask;
  if (maskBounds.has_value()) {
    qp.applyMaskBounds = true;
    qp.maskBounds = *maskBounds;
  }

  GeodeTextureEncoder::drawTexturedQuad(*impl_->device, *impl_->imagePipeline, impl_->pass, content,
                                        mvp, impl_->targetWidth, impl_->targetHeight, qp);
}

void GeoEncoder::drawImage(const svg::ImageResource& image, const Box2d& destRect, double opacity,
                           bool pixelated) {
  if (image.data.empty() || image.width <= 0 || image.height <= 0) {
    return;
  }
  if (destRect.isEmpty()) {
    return;
  }
  // Size cap: refuse pathological images. 16384 × 16384 × 4 bytes = 1 GiB,
  // which is already past any sensible WebGPU device limit. The texture
  // creation itself enforces tighter limits on the device side, but a sanity
  // check here turns "invalid texture descriptor → uncaptured device error"
  // into a clean no-op for the renderer.
  constexpr int kMaxImageDim = 16384;
  if (image.width > kMaxImageDim || image.height > kMaxImageDim) {
    return;
  }
  const size_t expectedBytes = static_cast<size_t>(image.width) *
                               static_cast<size_t>(image.height) * 4u;
  if (image.data.size() < expectedBytes) {
    return;
  }

  impl_->ensurePassOpen();
  impl_->bindImagePipeline(impl_->imagePipeline->pipeline());

  // Upload the image to a sampled texture.
  wgpu::Texture texture = GeodeTextureEncoder::uploadRgba8Texture(
      *impl_->device, image.data.data(), static_cast<uint32_t>(image.width),
      static_cast<uint32_t>(image.height));
  if (!texture) {
    return;
  }

  // Build the same MVP the Slug-fill path uses, so the image's local-space
  // destination rectangle lands in the correct spot after the current
  // transform.
  float mvp[16];
  impl_->buildMvp(mvp);

  GeodeTextureEncoder::QuadParams qp;
  qp.destRect = destRect;
  qp.srcRect = Box2d({0.0, 0.0}, {1.0, 1.0});
  qp.opacity = opacity;
  qp.filter = pixelated ? GeodeTextureEncoder::Filter::Nearest
                        : GeodeTextureEncoder::Filter::Linear;

  GeodeTextureEncoder::drawTexturedQuad(*impl_->device, *impl_->imagePipeline, impl_->pass,
                                        texture, mvp, impl_->targetWidth, impl_->targetHeight, qp);
}

void GeoEncoder::finish() {
  if (impl_->passOpen) {
    impl_->pass.end();
    impl_->passOpen = false;
  } else if (impl_->hasExplicitClear) {
    // No draws but a clear was requested — open and immediately close a pass
    // so the clear actually happens.
    impl_->ensurePassOpen();
    impl_->pass.end();
    impl_->passOpen = false;
  }

  wgpu::CommandBuffer cmdBuf = impl_->commandEncoder.finish();
  impl_->device->queue().submit(1, &cmdBuf);
}

}  // namespace donner::geode
