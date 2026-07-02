#include <memory>
#include <utility>

#include "donner/svg/renderer/RendererInternal.h"
#include "donner/svg/renderer/RendererTinySkia.h"

namespace donner::svg {

std::unique_ptr<RendererInterface> CreateRendererImplementation(bool verbose) {
  return std::make_unique<RendererTinySkia>(verbose);
}

std::unique_ptr<RendererInterface> CreateRendererImplementation(
    std::shared_ptr<geode::GeodeDevice> device, bool verbose) {
  // The tiny-skia backend is CPU-only; a provided GPU device is ignored.
  (void)device;
  return std::make_unique<RendererTinySkia>(verbose);
}

}  // namespace donner::svg
