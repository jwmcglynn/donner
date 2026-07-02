#include <memory>
#include <utility>

#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererInternal.h"

namespace donner::svg {

std::unique_ptr<RendererInterface> CreateRendererImplementation(bool verbose) {
  return std::make_unique<RendererGeode>(verbose);
}

std::unique_ptr<RendererInterface> CreateRendererImplementation(
    std::shared_ptr<geode::GeodeDevice> device, bool verbose) {
  return std::make_unique<RendererGeode>(std::move(device), verbose);
}

}  // namespace donner::svg
