#include "src/svg/renderer/renderer_utils.h"

#include "src/svg/components/document_context.h"
#include "src/svg/components/rendering_context.h"
#include "src/svg/registry/registry.h"

namespace donner::svg {

void RendererUtils::prepareDocumentForRendering(SVGDocument& document, bool verbose,
                                                std::vector<ParseError>* outWarnings) {
  Registry& registry = document.registry();
  if (!registry.ctx().contains<components::RenderingContext>()) {
    registry.ctx().emplace<components::RenderingContext>(registry);
  }

  registry.ctx().get<components::RenderingContext>().instantiateRenderTree(verbose, outWarnings);
}

}  // namespace donner::svg
