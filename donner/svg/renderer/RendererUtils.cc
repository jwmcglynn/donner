#include "donner/svg/renderer/RendererUtils.h"

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {

void RendererUtils::prepareDocumentForRendering(SVGDocument& document, bool verbose,
                                                ParseWarningSink& warningSink) {
  Registry& registry = document.registry();
  if (!registry.ctx().contains<components::RenderingContext>()) {
    registry.ctx().emplace<components::RenderingContext>(registry);
  }

  registry.ctx().get<components::RenderingContext>().instantiateRenderTree(verbose, warningSink);
}

}  // namespace donner::svg
