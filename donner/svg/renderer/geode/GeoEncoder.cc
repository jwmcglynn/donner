#include "donner/svg/renderer/geode/GeoEncoder.h"

#include <cstring>
#include <vector>

#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodePathEncoder.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"

namespace donner::geode {

namespace {

/// Round size up to a multiple of 4 (WebGPU requires buffer sizes to be
/// multiples of 4 for COPY_SRC/COPY_DST operations).
constexpr uint64_t roundUp4(uint64_t size) { return (size + 3u) & ~uint64_t{3u}; }

/// Layout of the per-draw uniform buffer (must match shaders/slug_fill.wgsl).
///
/// WGSL struct layout requires the total size to be a multiple of the largest
/// member alignment. With mat4x4 (16-byte alignment) and a vec3<u32> at the
/// end, the struct rounds to 128 bytes. We pad explicitly to keep this stable
/// across compilers / WGSL backends.
struct alignas(16) Uniforms {
  float mvp[16];      // 0  .. 64
  float viewport[2];  // 64 .. 72
  float _pad0[2];     // 72 .. 80   pad color to 80
  float color[4];     // 80 .. 96
  uint32_t fillRule;  // 96 .. 100
  uint32_t _pad1[7];  // 100 .. 128 pad struct to 128 (mat4 alignment)
};
static_assert(sizeof(Uniforms) == 128, "Uniforms struct layout mismatch");

}  // namespace

struct GeoEncoder::Impl {
  GeodeDevice* device;
  const GeodePipeline* pipeline;
  wgpu::Texture target;
  wgpu::TextureView targetView;
  wgpu::CommandEncoder commandEncoder;
  uint32_t targetWidth;
  uint32_t targetHeight;

  // Pending draws are recorded into a render pass that's lazily opened.
  // The first clear/fill triggers `beginPass()`; finish() ends it.
  bool passOpen = false;
  wgpu::RenderPassEncoder pass;

  // Default load op = clear-to-transparent until clear() is called explicitly.
  wgpu::Color clearColor = {0.0, 0.0, 0.0, 0.0};
  bool hasExplicitClear = false;

  // Current transform — applied to MVP for the next draw.
  Transform2d transform = Transform2d();  // Identity.

  /// Open the render pass on demand.
  void ensurePassOpen() {
    if (passOpen) {
      return;
    }
    wgpu::RenderPassColorAttachment color = {};
    color.view = targetView;
    color.loadOp = wgpu::LoadOp::Clear;
    color.storeOp = wgpu::StoreOp::Store;
    color.clearValue = clearColor;

    wgpu::RenderPassDescriptor desc = {};
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color;
    desc.label = "GeoEncoderPass";
    pass = commandEncoder.BeginRenderPass(&desc);
    pass.SetPipeline(pipeline->pipeline());
    passOpen = true;
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

GeoEncoder::GeoEncoder(GeodeDevice& device, const GeodePipeline& pipeline,
                       const wgpu::Texture& target)
    : impl_(std::make_unique<Impl>()) {
  impl_->device = &device;
  impl_->pipeline = &pipeline;
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

void GeoEncoder::fillPath(const Path& path, const css::RGBA& color, FillRule rule) {
  impl_->ensurePassOpen();

  // 1. CPU encode the path into Slug band data.
  EncodedPath encoded = GeodePathEncoder::encode(path, rule);
  if (encoded.empty()) {
    return;  // Nothing to draw.
  }

  const wgpu::Device& dev = impl_->device->device();
  const wgpu::Queue& queue = impl_->device->queue();

  // 2. Allocate and upload GPU buffers.
  // Vertex buffer.
  const uint64_t vbSize =
      roundUp4(encoded.vertices.size() * sizeof(EncodedPath::Vertex));
  wgpu::BufferDescriptor vbDesc = {};
  vbDesc.label = "GeodeVB";
  vbDesc.size = vbSize;
  vbDesc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer vb = dev.CreateBuffer(&vbDesc);
  queue.WriteBuffer(vb, 0, encoded.vertices.data(),
                    encoded.vertices.size() * sizeof(EncodedPath::Vertex));

  // Bands SSBO. Must be aligned to 16 bytes per WebGPU storage buffer rules.
  // Each Band is already 32 bytes (8 x 4-byte fields), so this is fine.
  const uint64_t bandsSize = roundUp4(encoded.bands.size() * sizeof(EncodedPath::Band));
  wgpu::BufferDescriptor bandsDesc = {};
  bandsDesc.label = "GeodeBandsSSBO";
  bandsDesc.size = bandsSize;
  bandsDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer bandsBuf = dev.CreateBuffer(&bandsDesc);
  queue.WriteBuffer(bandsBuf, 0, encoded.bands.data(),
                    encoded.bands.size() * sizeof(EncodedPath::Band));

  // Curves SSBO (flat float array).
  const uint64_t curveFloats = encoded.curves.size() * 6u;  // 6 floats per quadratic
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
  u.viewport[0] = static_cast<float>(impl_->targetWidth);
  u.viewport[1] = static_cast<float>(impl_->targetHeight);
  // Premultiply alpha for the blend pipeline.
  const float alpha = color.a / 255.0f;
  u.color[0] = (color.r / 255.0f) * alpha;
  u.color[1] = (color.g / 255.0f) * alpha;
  u.color[2] = (color.b / 255.0f) * alpha;
  u.color[3] = alpha;
  u.fillRule = (rule == FillRule::EvenOdd) ? 1u : 0u;

  wgpu::BufferDescriptor uniDesc = {};
  uniDesc.label = "GeodeUniforms";
  uniDesc.size = sizeof(Uniforms);
  uniDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer uniBuf = dev.CreateBuffer(&uniDesc);
  queue.WriteBuffer(uniBuf, 0, &u, sizeof(Uniforms));

  // 3. Bind group.
  wgpu::BindGroupEntry entries[3] = {};
  entries[0].binding = 0;
  entries[0].buffer = uniBuf;
  entries[0].size = sizeof(Uniforms);
  entries[1].binding = 1;
  entries[1].buffer = bandsBuf;
  entries[1].size = bandsSize;
  entries[2].binding = 2;
  entries[2].buffer = curvesBuf;
  entries[2].size = curvesSize;

  wgpu::BindGroupDescriptor bgDesc = {};
  bgDesc.label = "GeodeBindGroup";
  bgDesc.layout = impl_->pipeline->bindGroupLayout();
  bgDesc.entryCount = 3;
  bgDesc.entries = entries;
  wgpu::BindGroup bindGroup = dev.CreateBindGroup(&bgDesc);

  // 4. Record the draw call.
  impl_->pass.SetVertexBuffer(0, vb, 0, vbSize);
  impl_->pass.SetBindGroup(0, bindGroup);
  impl_->pass.Draw(static_cast<uint32_t>(encoded.vertices.size()), 1, 0, 0);
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
