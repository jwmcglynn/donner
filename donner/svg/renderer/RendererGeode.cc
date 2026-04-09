#include "donner/svg/renderer/RendererGeode.h"

#include <webgpu/webgpu_cpp.h>

#include <iostream>
#include <vector>

#include "donner/base/Path.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/geode/GeoEncoder.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"

namespace donner::svg {

namespace {

constexpr wgpu::TextureFormat kFormat = wgpu::TextureFormat::RGBA8Unorm;

/// WebGPU requires bytesPerRow alignment to 256 when copying textures to
/// buffers. This rounds the unpadded row width up to the next 256 boundary.
constexpr uint32_t alignBytesPerRow(uint32_t unpadded) {
  constexpr uint32_t kAlign = 256u;
  return (unpadded + kAlign - 1u) & ~(kAlign - 1u);
}

}  // namespace

struct RendererGeode::Impl {
  bool verbose = false;

  // GPU resources. Created in the constructor; if device creation fails,
  // `device` is null and the renderer enters a no-op state.
  std::unique_ptr<geode::GeodeDevice> device;
  std::unique_ptr<geode::GeodePipeline> pipeline;

  // Per-frame resources, recreated in `beginFrame`.
  RenderViewport viewport;
  int pixelWidth = 0;
  int pixelHeight = 0;
  wgpu::Texture target;  // RGBA8 RenderAttachment | CopySrc
  std::unique_ptr<geode::GeoEncoder> encoder;

  // CPU-side state.
  PaintParams paint;
  Transform2d currentTransform;
  std::vector<Transform2d> transformStack;

  // Stub-state depth counters — incremented on push, decremented on pop. Used
  // only to keep stack semantics balanced and to drop the warning to stderr
  // exactly once per category in verbose mode.
  bool warnedClip = false;
  bool warnedLayer = false;
  bool warnedFilter = false;
  bool warnedMask = false;
  bool warnedPattern = false;
  bool warnedStroke = false;
  bool warnedGradient = false;
  bool warnedImage = false;
  bool warnedText = false;

  /// Resolve the current `paint_.fill` to a solid RGBA color, or nullopt if
  /// the fill is None or a paint server we don't yet support.
  std::optional<css::RGBA> resolveSolidFill() {
    if (std::holds_alternative<PaintServer::None>(paint.fill)) {
      return std::nullopt;
    }
    const css::RGBA currentColor = paint.currentColor.rgba();
    const float fillOpacity = static_cast<float>(paint.fillOpacity * paint.opacity);
    if (const auto* solid = std::get_if<PaintServer::Solid>(&paint.fill)) {
      return solid->color.resolve(currentColor, fillOpacity);
    }
    if (verbose && !warnedGradient) {
      std::cerr << "RendererGeode: gradient/pattern paint servers not yet supported\n";
      warnedGradient = true;
    }
    return std::nullopt;
  }

  /// Push the renderer's currentTransform onto the encoder before drawing.
  void syncTransform() {
    if (encoder) {
      encoder->setTransform(currentTransform);
    }
  }

  /// Issue a fill of the given path. Resolves the current paint and skips if
  /// the fill is non-solid or empty.
  void fillResolved(const Path& path, FillRule rule) {
    if (!encoder) {
      return;
    }
    auto color = resolveSolidFill();
    if (!color.has_value()) {
      return;
    }
    syncTransform();
    encoder->fillPath(path, *color, rule);
  }
};

RendererGeode::RendererGeode(bool verbose) : impl_(std::make_unique<Impl>()) {
  impl_->verbose = verbose;
  impl_->device = geode::GeodeDevice::CreateHeadless();
  if (!impl_->device) {
    if (verbose) {
      std::cerr << "RendererGeode: GeodeDevice::CreateHeadless() failed — entering no-op mode\n";
    }
    return;
  }
  impl_->pipeline = std::make_unique<geode::GeodePipeline>(impl_->device->device(), kFormat);
}

RendererGeode::~RendererGeode() = default;
RendererGeode::RendererGeode(RendererGeode&&) noexcept = default;
RendererGeode& RendererGeode::operator=(RendererGeode&&) noexcept = default;

void RendererGeode::draw(SVGDocument& document) {
  RendererDriver driver(*this, impl_->verbose);
  driver.draw(document);
}

int RendererGeode::width() const { return impl_->pixelWidth; }
int RendererGeode::height() const { return impl_->pixelHeight; }

void RendererGeode::beginFrame(const RenderViewport& viewport) {
  impl_->viewport = viewport;
  impl_->pixelWidth = static_cast<int>(viewport.size.x * viewport.devicePixelRatio);
  impl_->pixelHeight = static_cast<int>(viewport.size.y * viewport.devicePixelRatio);
  impl_->currentTransform = Transform2d();
  impl_->transformStack.clear();
  impl_->paint = PaintParams();
  impl_->encoder.reset();
  impl_->target = wgpu::Texture();

  if (!impl_->device || !impl_->pipeline || impl_->pixelWidth <= 0 || impl_->pixelHeight <= 0) {
    return;
  }

  wgpu::TextureDescriptor td = {};
  td.label = "RendererGeodeTarget";
  td.size = {static_cast<uint32_t>(impl_->pixelWidth),
             static_cast<uint32_t>(impl_->pixelHeight), 1};
  td.format = kFormat;
  td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  td.dimension = wgpu::TextureDimension::e2D;
  impl_->target = impl_->device->device().CreateTexture(&td);

  impl_->encoder =
      std::make_unique<geode::GeoEncoder>(*impl_->device, *impl_->pipeline, impl_->target);
  // Default to a transparent clear so an empty frame matches the other
  // backends' "no document content" appearance.
  impl_->encoder->clear(css::RGBA(0, 0, 0, 0));
}

void RendererGeode::endFrame() {
  if (impl_->encoder) {
    impl_->encoder->finish();
    impl_->encoder.reset();
  }
  impl_->currentTransform = Transform2d();
  impl_->transformStack.clear();
}

void RendererGeode::setTransform(const Transform2d& transform) {
  impl_->currentTransform = transform;
}

void RendererGeode::pushTransform(const Transform2d& transform) {
  impl_->transformStack.push_back(impl_->currentTransform);
  impl_->currentTransform = transform * impl_->currentTransform;
}

void RendererGeode::popTransform() {
  if (impl_->transformStack.empty()) {
    return;
  }
  impl_->currentTransform = impl_->transformStack.back();
  impl_->transformStack.pop_back();
}

void RendererGeode::pushClip(const ResolvedClip& /*clip*/) {
  if (impl_->verbose && !impl_->warnedClip) {
    std::cerr << "RendererGeode: clipping not yet implemented (Phase 3)\n";
    impl_->warnedClip = true;
  }
}

void RendererGeode::popClip() {}

void RendererGeode::pushIsolatedLayer(double /*opacity*/, MixBlendMode /*blendMode*/) {
  if (impl_->verbose && !impl_->warnedLayer) {
    std::cerr << "RendererGeode: isolated layers not yet implemented (Phase 3)\n";
    impl_->warnedLayer = true;
  }
}

void RendererGeode::popIsolatedLayer() {}

void RendererGeode::pushFilterLayer(const components::FilterGraph& /*filterGraph*/,
                                    const std::optional<Box2d>& /*filterRegion*/) {
  if (impl_->verbose && !impl_->warnedFilter) {
    std::cerr << "RendererGeode: filter layers not yet implemented (Phase 7)\n";
    impl_->warnedFilter = true;
  }
}

void RendererGeode::popFilterLayer() {}

void RendererGeode::pushMask(const std::optional<Box2d>& /*maskBounds*/) {
  if (impl_->verbose && !impl_->warnedMask) {
    std::cerr << "RendererGeode: masks not yet implemented (Phase 3)\n";
    impl_->warnedMask = true;
  }
}

void RendererGeode::transitionMaskToContent() {}
void RendererGeode::popMask() {}

void RendererGeode::beginPatternTile(const Box2d& /*tileRect*/,
                                     const Transform2d& /*targetFromPattern*/) {
  if (impl_->verbose && !impl_->warnedPattern) {
    std::cerr << "RendererGeode: pattern tiles not yet implemented (Phase 2)\n";
    impl_->warnedPattern = true;
  }
}

void RendererGeode::endPatternTile(bool /*forStroke*/) {}

void RendererGeode::setPaint(const PaintParams& paint) { impl_->paint = paint; }

void RendererGeode::drawPath(const PathShape& path, const StrokeParams& stroke) {
  impl_->fillResolved(path.path, path.fillRule);

  if (stroke.strokeWidth > 0.0 &&
      !std::holds_alternative<PaintServer::None>(impl_->paint.stroke)) {
    if (impl_->verbose && !impl_->warnedStroke) {
      std::cerr << "RendererGeode: stroke rendering not yet implemented (Phase 2)\n";
      impl_->warnedStroke = true;
    }
  }
}

void RendererGeode::drawRect(const Box2d& rect, const StrokeParams& stroke) {
  Path path = PathBuilder().addRect(rect).build();
  PathShape shape{std::move(path), FillRule::NonZero, Transform2d(), 0};
  drawPath(shape, stroke);
}

void RendererGeode::drawEllipse(const Box2d& bounds, const StrokeParams& stroke) {
  Path path = PathBuilder().addEllipse(bounds).build();
  PathShape shape{std::move(path), FillRule::NonZero, Transform2d(), 0};
  drawPath(shape, stroke);
}

void RendererGeode::drawImage(const ImageResource& /*image*/, const ImageParams& /*params*/) {
  if (impl_->verbose && !impl_->warnedImage) {
    std::cerr << "RendererGeode: drawImage not yet implemented (Phase 2)\n";
    impl_->warnedImage = true;
  }
}

void RendererGeode::drawText(Registry& /*registry*/,
                             const components::ComputedTextComponent& /*text*/,
                             const TextParams& /*params*/) {
  if (impl_->verbose && !impl_->warnedText) {
    std::cerr << "RendererGeode: text rendering not yet implemented (Phase 4)\n";
    impl_->warnedText = true;
  }
}

RendererBitmap RendererGeode::takeSnapshot() const {
  RendererBitmap bitmap;
  if (!impl_->device || !impl_->target || impl_->pixelWidth <= 0 || impl_->pixelHeight <= 0) {
    return bitmap;
  }

  const uint32_t width = static_cast<uint32_t>(impl_->pixelWidth);
  const uint32_t height = static_cast<uint32_t>(impl_->pixelHeight);
  const uint32_t bytesPerRow = alignBytesPerRow(width * 4u);

  // Allocate readback buffer.
  wgpu::BufferDescriptor bd = {};
  bd.label = "RendererGeodeReadback";
  bd.size = static_cast<uint64_t>(bytesPerRow) * static_cast<uint64_t>(height);
  bd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  wgpu::Buffer readback = impl_->device->device().CreateBuffer(&bd);

  // Copy texture → readback buffer.
  wgpu::CommandEncoder enc = impl_->device->device().CreateCommandEncoder();
  wgpu::TexelCopyTextureInfo src = {};
  src.texture = impl_->target;
  src.mipLevel = 0;
  src.origin = {0, 0, 0};
  wgpu::TexelCopyBufferInfo dst = {};
  dst.buffer = readback;
  dst.layout.bytesPerRow = bytesPerRow;
  dst.layout.rowsPerImage = height;
  wgpu::Extent3D copySize = {width, height, 1};
  enc.CopyTextureToBuffer(&src, &dst, &copySize);

  wgpu::CommandBuffer cmd = enc.Finish();
  impl_->device->queue().Submit(1, &cmd);

  // Map for read.
  bool mapDone = false;
  bool mapOk = false;
  readback.MapAsync(wgpu::MapMode::Read, 0, bd.size, wgpu::CallbackMode::AllowSpontaneous,
                    [&](wgpu::MapAsyncStatus status, wgpu::StringView /*msg*/) {
                      mapOk = (status == wgpu::MapAsyncStatus::Success);
                      mapDone = true;
                    });
  while (!mapDone) {
    impl_->device->device().Tick();
  }
  if (!mapOk) {
    return bitmap;
  }

  const uint8_t* mapped = static_cast<const uint8_t*>(readback.GetConstMappedRange());

  // Strip row padding so the consumer gets a tightly packed RGBA buffer.
  bitmap.dimensions = Vector2i(static_cast<int>(width), static_cast<int>(height));
  bitmap.rowBytes = static_cast<size_t>(width) * 4u;
  bitmap.pixels.resize(bitmap.rowBytes * height);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* row = mapped + static_cast<size_t>(y) * bytesPerRow;
    std::copy_n(row, bitmap.rowBytes, bitmap.pixels.data() + static_cast<size_t>(y) * bitmap.rowBytes);
  }
  readback.Unmap();
  return bitmap;
}

}  // namespace donner::svg
