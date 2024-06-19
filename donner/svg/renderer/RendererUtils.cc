#include "donner/svg/renderer/RendererUtils.h"

#include "donner/svg/components/DocumentContext.h"
#include "donner/svg/components/RenderingContext.h"
#include "donner/svg/registry/Registry.h"

namespace donner::svg {

void RendererUtils::prepareDocumentForRendering(SVGDocument& document, bool verbose,
                                                std::vector<parser::ParseError>* outWarnings) {
  Registry& registry = document.registry();
  if (!registry.ctx().contains<components::RenderingContext>()) {
    registry.ctx().emplace<components::RenderingContext>(registry);
  }

  registry.ctx().get<components::RenderingContext>().instantiateRenderTree(verbose, outWarnings);
}

}  // namespace donner::svg
