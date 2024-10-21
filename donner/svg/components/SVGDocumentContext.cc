#include "donner/svg/components/SVGDocumentContext.h"

namespace donner::svg::components {

SVGDocumentContext::SVGDocumentContext(InternalCtorTag, const std::shared_ptr<Registry>& registry)
    : registry_(registry) {
  registry->on_construct<IdComponent>().connect<&SVGDocumentContext::onIdSet>(this);
  registry->on_destroy<IdComponent>().connect<&SVGDocumentContext::onIdDestroy>(this);
}

}  // namespace donner::svg::components
