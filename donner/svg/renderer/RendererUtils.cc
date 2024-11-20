#include "donner/svg/renderer/RendererUtils.h"

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {

void RendererUtils::prepareDocumentForRendering(SVGDocument& document, bool verbose,
                                                std::vector<ParseError>* outWarnings) {
  Registry& registry = document.registry();
  if (!registry.ctx().contains<components::RenderingContext>()) {
    registry.ctx().emplace<components::RenderingContext>(registry);
  }

  registry.ctx().get<components::ResourceManagerContext>().loadResources(outWarnings);
  registry.ctx().get<components::RenderingContext>().instantiateRenderTree(verbose, outWarnings);
}

}  // namespace donner::svg
