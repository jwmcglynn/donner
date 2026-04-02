#include <memory>

#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/RendererTinySkia.h"

namespace donner::svg {

std::unique_ptr<RendererInterface> CreateRendererInterface(bool verbose) {
  return std::make_unique<RendererTinySkia>(verbose);
}

}  // namespace donner::svg
