#include <memory>

#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/RendererSkia.h"

namespace donner::svg {

std::unique_ptr<RendererInterface> CreateRendererInterface(bool verbose) {
  return std::make_unique<RendererSkia>(verbose);
}

}  // namespace donner::svg
