#include "donner/svg/renderer/geode/GeoEncoder.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodePathEncoder.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/renderer/geode/GeodeTextureEncoder.h"
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
  uint32_t _pad0;             // 172 .. 176 pad struct to 176 (mat4 alignment)
};
static_assert(sizeof(Uniforms) == 176, "Uniforms struct layout mismatch");

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
};
static_assert(sizeof(GradientUniforms) == 480,
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

  /// Lazily create the dummy texture + sampler used by the solid-fill path.
  void ensureDummyResources() {
    if (dummyTextureView) {
      return;
    }
    const wgpu::Device& dev = device->device();

    wgpu::TextureDescriptor td = {};
    td.label = "GeoEncoderDummyPattern";
    td.size = {1u, 1u, 1u};
    td.format = wgpu::TextureFormat::RGBA8Unorm;
    td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.dimension = wgpu::TextureDimension::e2D;
    dummyTexture = dev.CreateTexture(&td);

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
    device->queue().WriteTexture(&dst, pixel, sizeof(pixel), &layout, &extent);

    dummyTextureView = dummyTexture.CreateView();

    wgpu::SamplerDescriptor sd = {};
    sd.label = "GeoEncoderDummySampler";
    sd.addressModeU = wgpu::AddressMode::Repeat;
    sd.addressModeV = wgpu::AddressMode::Repeat;
    sd.minFilter = wgpu::FilterMode::Linear;
    sd.magFilter = wgpu::FilterMode::Linear;
    dummySampler = dev.CreateSampler(&sd);
  }

  /// Open the render pass on demand.
  void ensurePassOpen() {
    if (passOpen) {
      return;
    }
    wgpu::RenderPassColorAttachment color = {};
    color.view = targetView;
    color.loadOp = loadPreserve ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
    color.storeOp = wgpu::StoreOp::Store;
    color.clearValue = clearColor;

    wgpu::RenderPassDescriptor desc = {};
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color;
    desc.label = "GeoEncoderPass";
    pass = commandEncoder.BeginRenderPass(&desc);
    // Pipelines are set per-draw — `fillPath` / `fillPathLinearGradient` /
    // `drawImage` each rebind their own pipeline before issuing a draw call.
    // Re-binding only happens when the bound pipeline differs from the next
    // one (see bindSolidPipeline / bindGradientPipeline / bindImagePipeline).
    currentPipeline = BoundPipeline::kNone;
    currentPipelineIsGradient = false;
    passOpen = true;
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
      pass.SetPipeline(pipeline->pipeline());
      currentPipeline = BoundPipeline::kSolid;
      currentPipelineIsGradient = false;
    }
  }
  void bindGradientPipeline() {
    if (currentPipeline != BoundPipeline::kGradient) {
      pass.SetPipeline(gradientPipeline->pipeline());
      currentPipeline = BoundPipeline::kGradient;
      currentPipelineIsGradient = true;
    }
  }
  void bindImagePipeline(const wgpu::RenderPipeline& imageRenderPipeline) {
    if (currentPipeline != BoundPipeline::kImage) {
      pass.SetPipeline(imageRenderPipeline);
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
                       const GeodeImagePipeline& imagePipeline, const wgpu::Texture& target)
    : impl_(std::make_unique<Impl>()) {
  impl_->device = &device;
  impl_->pipeline = &fillPipeline;
  impl_->gradientPipeline = &gradientPipeline;
  impl_->imagePipeline = &imagePipeline;
  impl_->target = target;
  impl_->targetView = target.CreateView();
  impl_->targetWidth = target.GetWidth();
  impl_->targetHeight = target.GetHeight();

  wgpu::CommandEncoderDescriptor desc = {};
  desc.label = "GeoEncoder";
  impl_->commandEncoder = device.device().CreateCommandEncoder(&desc);
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
  wgpu::SamplerDescriptor sd = {};
  sd.label = "GeoEncoderPatternSampler";
  sd.addressModeU = wgpu::AddressMode::Repeat;
  sd.addressModeV = wgpu::AddressMode::Repeat;
  sd.minFilter = wgpu::FilterMode::Linear;
  sd.magFilter = wgpu::FilterMode::Linear;
  wgpu::Sampler sampler = impl_->device->device().CreateSampler(&sd);

  FillDrawArgs args = {};
  args.path = &path;
  args.rule = rule;
  args.paintMode = 1u;
  args.patternView = paint.tile.CreateView();
  args.patternSampler = sampler;
  args.patternFromPath = paint.patternFromPath;
  args.tileSize = paint.tileSize;
  args.patternOpacity = static_cast<float>(paint.opacity);

  submitFillDraw(args);
}

void GeoEncoder::submitFillDraw(const FillDrawArgs& args) {
  impl_->ensurePassOpen();
  impl_->bindSolidPipeline();

  // The pass shares the Slug fill pipeline and the image-blit pipeline;
  // always set this path's pipeline before issuing the draw so a preceding
  // `drawImage` (or any future pipeline-switching helper) doesn't leak state.
  impl_->pass.SetPipeline(impl_->pipeline->pipeline());

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
  vbDesc.label = "GeodeVB";
  vbDesc.size = vbSize;
  vbDesc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer vb = dev.CreateBuffer(&vbDesc);
  queue.WriteBuffer(vb, 0, encoded.vertices.data(),
                    encoded.vertices.size() * sizeof(EncodedPath::Vertex));

  const uint64_t bandsSize = roundUp4(encoded.bands.size() * sizeof(EncodedPath::Band));
  wgpu::BufferDescriptor bandsDesc = {};
  bandsDesc.label = "GeodeBandsSSBO";
  bandsDesc.size = bandsSize;
  bandsDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer bandsBuf = dev.CreateBuffer(&bandsDesc);
  queue.WriteBuffer(bandsBuf, 0, encoded.bands.data(),
                    encoded.bands.size() * sizeof(EncodedPath::Band));

  const uint64_t curveFloats = encoded.curves.size() * 6u;
  const uint64_t curvesSize = roundUp4(curveFloats * sizeof(float));
  wgpu::BufferDescriptor curvesDesc = {};
  curvesDesc.label = "GeodeCurvesSSBO";
  curvesDesc.size = curvesSize;
  curvesDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer curvesBuf = dev.CreateBuffer(&curvesDesc);
  queue.WriteBuffer(curvesBuf, 0, encoded.curves.data(),
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
  u._pad0 = 0u;

  wgpu::BufferDescriptor uniDesc = {};
  uniDesc.label = "GeodeUniforms";
  uniDesc.size = sizeof(Uniforms);
  uniDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer uniBuf = dev.CreateBuffer(&uniDesc);
  queue.WriteBuffer(uniBuf, 0, &u, sizeof(Uniforms));

  // 3. Bind group — five entries: uniforms, bands SSBO, curves SSBO,
  // pattern texture, pattern sampler. Solid-fill draws bind the dummy
  // texture / sampler; the shader skips sampling when paintMode == 0.
  wgpu::BindGroupEntry entries[5] = {};
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

  wgpu::BindGroupDescriptor bgDesc = {};
  bgDesc.label = "GeodeBindGroup";
  bgDesc.layout = impl_->pipeline->bindGroupLayout();
  bgDesc.entryCount = 5;
  bgDesc.entries = entries;
  wgpu::BindGroup bindGroup = dev.CreateBindGroup(&bgDesc);

  // 4. Record the draw call.
  impl_->pass.SetVertexBuffer(0, vb, 0, vbSize);
  impl_->pass.SetBindGroup(0, bindGroup);
  impl_->pass.Draw(static_cast<uint32_t>(encoded.vertices.size()), 1, 0, 0);
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

  // Premultiply each stop's RGB by A for the blend pipeline. Clamp the stop
  // count to the shader's hard cap; overflow was warned about at the caller.
  const uint32_t stopCount =
      std::min<uint32_t>(kMaxGradientStops, static_cast<uint32_t>(stops.size()));
  u.stopCount = stopCount;
  for (uint32_t i = 0; i < stopCount; ++i) {
    const StopT& s = stops[i];
    const float a = s.rgba[3];
    u.stopColors[i * 4 + 0] = s.rgba[0] * a;
    u.stopColors[i * 4 + 1] = s.rgba[1] * a;
    u.stopColors[i * 4 + 2] = s.rgba[2] * a;
    u.stopColors[i * 4 + 3] = a;
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
  vbDesc.label = "GeodeGradientVB";
  vbDesc.size = vbSize;
  vbDesc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer vb = dev.CreateBuffer(&vbDesc);
  queue.WriteBuffer(vb, 0, encoded.vertices.data(),
                    encoded.vertices.size() * sizeof(EncodedPath::Vertex));

  const uint64_t bandsSize = roundUp4(encoded.bands.size() * sizeof(EncodedPath::Band));
  wgpu::BufferDescriptor bandsDesc = {};
  bandsDesc.label = "GeodeGradientBandsSSBO";
  bandsDesc.size = bandsSize;
  bandsDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer bandsBuf = dev.CreateBuffer(&bandsDesc);
  queue.WriteBuffer(bandsBuf, 0, encoded.bands.data(),
                    encoded.bands.size() * sizeof(EncodedPath::Band));

  const uint64_t curveFloats = encoded.curves.size() * 6u;
  const uint64_t curvesSize = roundUp4(curveFloats * sizeof(float));
  wgpu::BufferDescriptor curvesDesc = {};
  curvesDesc.label = "GeodeGradientCurvesSSBO";
  curvesDesc.size = curvesSize;
  curvesDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer curvesBuf = dev.CreateBuffer(&curvesDesc);
  queue.WriteBuffer(curvesBuf, 0, encoded.curves.data(),
                    encoded.curves.size() * sizeof(EncodedPath::Curve));

  // 3. Build gradient uniforms.
  GradientUniforms u = {};
  impl_->buildMvp(u.mvp);
  u.viewport[0] = static_cast<float>(impl_->targetWidth);
  u.viewport[1] = static_cast<float>(impl_->targetHeight);
  populateSharedGradientUniforms<LinearGradientParams::Stop>(
      u, params.gradientFromPath, params.spreadMode, params.stops, rule);
  u.gradientKind = kGradientKindLinear;

  u.startGrad[0] = static_cast<float>(params.startGrad.x);
  u.startGrad[1] = static_cast<float>(params.startGrad.y);
  u.endGrad[0] = static_cast<float>(params.endGrad.x);
  u.endGrad[1] = static_cast<float>(params.endGrad.y);

  wgpu::BufferDescriptor uniDesc = {};
  uniDesc.label = "GeodeGradientUniforms";
  uniDesc.size = sizeof(GradientUniforms);
  uniDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer uniBuf = dev.CreateBuffer(&uniDesc);
  queue.WriteBuffer(uniBuf, 0, &u, sizeof(GradientUniforms));

  // 4. Bind group (same shape as the solid pipeline: uniform + 2 SSBOs).
  wgpu::BindGroupEntry entries[3] = {};
  entries[0].binding = 0;
  entries[0].buffer = uniBuf;
  entries[0].size = sizeof(GradientUniforms);
  entries[1].binding = 1;
  entries[1].buffer = bandsBuf;
  entries[1].size = bandsSize;
  entries[2].binding = 2;
  entries[2].buffer = curvesBuf;
  entries[2].size = curvesSize;

  wgpu::BindGroupDescriptor bgDesc = {};
  bgDesc.label = "GeodeGradientBindGroup";
  bgDesc.layout = impl_->gradientPipeline->bindGroupLayout();
  bgDesc.entryCount = 3;
  bgDesc.entries = entries;
  wgpu::BindGroup bindGroup = dev.CreateBindGroup(&bgDesc);

  impl_->pass.SetVertexBuffer(0, vb, 0, vbSize);
  impl_->pass.SetBindGroup(0, bindGroup);
  impl_->pass.Draw(static_cast<uint32_t>(encoded.vertices.size()), 1, 0, 0);
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
  vbDesc.label = "GeodeRadialGradientVB";
  vbDesc.size = vbSize;
  vbDesc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer vb = dev.CreateBuffer(&vbDesc);
  queue.WriteBuffer(vb, 0, encoded.vertices.data(),
                    encoded.vertices.size() * sizeof(EncodedPath::Vertex));

  const uint64_t bandsSize = roundUp4(encoded.bands.size() * sizeof(EncodedPath::Band));
  wgpu::BufferDescriptor bandsDesc = {};
  bandsDesc.label = "GeodeRadialGradientBandsSSBO";
  bandsDesc.size = bandsSize;
  bandsDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer bandsBuf = dev.CreateBuffer(&bandsDesc);
  queue.WriteBuffer(bandsBuf, 0, encoded.bands.data(),
                    encoded.bands.size() * sizeof(EncodedPath::Band));

  const uint64_t curveFloats = encoded.curves.size() * 6u;
  const uint64_t curvesSize = roundUp4(curveFloats * sizeof(float));
  wgpu::BufferDescriptor curvesDesc = {};
  curvesDesc.label = "GeodeRadialGradientCurvesSSBO";
  curvesDesc.size = curvesSize;
  curvesDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer curvesBuf = dev.CreateBuffer(&curvesDesc);
  queue.WriteBuffer(curvesBuf, 0, encoded.curves.data(),
                    encoded.curves.size() * sizeof(EncodedPath::Curve));

  GradientUniforms u = {};
  impl_->buildMvp(u.mvp);
  u.viewport[0] = static_cast<float>(impl_->targetWidth);
  u.viewport[1] = static_cast<float>(impl_->targetHeight);
  populateSharedGradientUniforms<RadialGradientParams::Stop>(
      u, params.gradientFromPath, params.spreadMode, params.stops, rule);
  u.gradientKind = kGradientKindRadial;

  u.radialCenter[0] = static_cast<float>(params.center.x);
  u.radialCenter[1] = static_cast<float>(params.center.y);
  u.radialFocal[0] = static_cast<float>(params.focalCenter.x);
  u.radialFocal[1] = static_cast<float>(params.focalCenter.y);
  u.radialRadius = static_cast<float>(params.radius);
  u.radialFocalRadius = static_cast<float>(params.focalRadius);

  wgpu::BufferDescriptor uniDesc = {};
  uniDesc.label = "GeodeRadialGradientUniforms";
  uniDesc.size = sizeof(GradientUniforms);
  uniDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer uniBuf = dev.CreateBuffer(&uniDesc);
  queue.WriteBuffer(uniBuf, 0, &u, sizeof(GradientUniforms));

  wgpu::BindGroupEntry entries[3] = {};
  entries[0].binding = 0;
  entries[0].buffer = uniBuf;
  entries[0].size = sizeof(GradientUniforms);
  entries[1].binding = 1;
  entries[1].buffer = bandsBuf;
  entries[1].size = bandsSize;
  entries[2].binding = 2;
  entries[2].buffer = curvesBuf;
  entries[2].size = curvesSize;

  wgpu::BindGroupDescriptor bgDesc = {};
  bgDesc.label = "GeodeRadialGradientBindGroup";
  bgDesc.layout = impl_->gradientPipeline->bindGroupLayout();
  bgDesc.entryCount = 3;
  bgDesc.entries = entries;
  wgpu::BindGroup bindGroup = dev.CreateBindGroup(&bgDesc);

  impl_->pass.SetVertexBuffer(0, vb, 0, vbSize);
  impl_->pass.SetBindGroup(0, bindGroup);
  impl_->pass.Draw(static_cast<uint32_t>(encoded.vertices.size()), 1, 0, 0);
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
    impl_->pass.End();
    impl_->passOpen = false;
  } else if (impl_->hasExplicitClear) {
    // No draws but a clear was requested — open and immediately close a pass
    // so the clear actually happens.
    impl_->ensurePassOpen();
    impl_->pass.End();
    impl_->passOpen = false;
  }

  wgpu::CommandBuffer cmdBuf = impl_->commandEncoder.Finish();
  impl_->device->queue().Submit(1, &cmdBuf);
}

}  // namespace donner::geode
