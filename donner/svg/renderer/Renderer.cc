#include "donner/svg/renderer/Renderer.h"

#include <cstddef>

#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/RendererInternal.h"

namespace donner::svg {

Renderer::Renderer(bool verbose) : impl_(CreateRendererImplementation(verbose)) {}

Renderer::~Renderer() = default;

Renderer::Renderer(Renderer&&) noexcept = default;

Renderer& Renderer::operator=(Renderer&&) noexcept = default;

void Renderer::draw(SVGDocument& document) {
  impl_->draw(document);
}

void Renderer::beginFrame(const RenderViewport& viewport) {
  impl_->beginFrame(viewport);
}

void Renderer::endFrame() {
  impl_->endFrame();
}

void Renderer::setTransform(const Transformd& transform) {
  impl_->setTransform(transform);
}

void Renderer::pushTransform(const Transformd& transform) {
  impl_->pushTransform(transform);
}

void Renderer::popTransform() {
  impl_->popTransform();
}

void Renderer::pushClip(const ResolvedClip& clip) {
  impl_->pushClip(clip);
}

void Renderer::popClip() {
  impl_->popClip();
}

void Renderer::pushIsolatedLayer(double opacity) {
  impl_->pushIsolatedLayer(opacity);
}

void Renderer::popIsolatedLayer() {
  impl_->popIsolatedLayer();
}

void Renderer::pushFilterLayer(const components::FilterGraph& filterGraph,
                               const std::optional<Boxd>& filterRegion) {
  impl_->pushFilterLayer(filterGraph, filterRegion);
}

void Renderer::popFilterLayer() {
  impl_->popFilterLayer();
}

void Renderer::pushMask(const std::optional<Boxd>& maskBounds) {
  impl_->pushMask(maskBounds);
}

void Renderer::transitionMaskToContent() {
  impl_->transitionMaskToContent();
}

void Renderer::popMask() {
  impl_->popMask();
}

void Renderer::beginPatternTile(const Boxd& tileRect, const Transformd& targetFromPattern) {
  impl_->beginPatternTile(tileRect, targetFromPattern);
}

void Renderer::endPatternTile(bool forStroke) {
  impl_->endPatternTile(forStroke);
}

void Renderer::setPaint(const PaintParams& paint) {
  impl_->setPaint(paint);
}

void Renderer::drawPath(const PathShape& path, const StrokeParams& stroke) {
  impl_->drawPath(path, stroke);
}

void Renderer::drawRect(const Boxd& rect, const StrokeParams& stroke) {
  impl_->drawRect(rect, stroke);
}

void Renderer::drawEllipse(const Boxd& bounds, const StrokeParams& stroke) {
  impl_->drawEllipse(bounds, stroke);
}

void Renderer::drawImage(const ImageResource& image, const ImageParams& params) {
  impl_->drawImage(image, params);
}

void Renderer::drawText(const components::ComputedTextComponent& text, const TextParams& params) {
  impl_->drawText(text, params);
}

RendererBitmap Renderer::takeSnapshot() const {
  return impl_->takeSnapshot();
}

bool Renderer::save(const char* filename) {
  const RendererBitmap snapshot = takeSnapshot();
  if (snapshot.empty()) {
    return false;
  }

  const std::size_t strideInPixels = snapshot.rowBytes / 4u;
  return RendererImageIO::writeRgbaPixelsToPngFile(filename, snapshot.pixels, snapshot.dimensions.x,
                                                   snapshot.dimensions.y, strideInPixels);
}

int Renderer::width() const {
  return impl_->width();
}

int Renderer::height() const {
  return impl_->height();
}

}  // namespace donner::svg
