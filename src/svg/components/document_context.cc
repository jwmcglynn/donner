#include "src/svg/components/document_context.h"

namespace donner::svg {

DocumentContext::DocumentContext(SVGDocument& document, Registry& registry) : document_(document) {
  registry.on_construct<IdComponent>().connect<&DocumentContext::onIdSet>(this);
  registry.on_destroy<IdComponent>().connect<&DocumentContext::onIdDestroy>(this);
}

}  // namespace donner::svg
