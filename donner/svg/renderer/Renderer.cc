#include "donner/svg/renderer/Renderer.h"

#include <cstddef>

#include "donner/svg/renderer/RendererImageIO.h"

namespace donner::svg {

std::unique_ptr<RendererInterface> CreateRendererInterface(bool verbose);

Renderer::Renderer(bool verbose) : impl_(CreateRendererInterface(verbose)) {}

Renderer::~Renderer() = default;

Renderer::Renderer(Renderer&&) noexcept = default;

Renderer& Renderer::operator=(Renderer&&) noexcept = default;

void Renderer::draw(SVGDocument& document) {
  impl_->draw(document);
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
