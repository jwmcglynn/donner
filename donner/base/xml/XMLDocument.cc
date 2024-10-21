#include "donner/base/xml/XMLDocument.h"

#include "donner/base/xml/components/XMLDocumentContext.h"
#include "donner/base/xml/components/XMLNamespaceContext.h"

namespace donner::xml {

using components::XMLDocumentContext;
using components::XMLNamespaceContext;

XMLDocument::XMLDocument() : registry_(std::make_shared<Registry>()) {
  auto& ctx = registry_->ctx().emplace<XMLDocumentContext>(XMLDocumentContext::InternalCtorTag{},
                                                           registry_);
  ctx.rootEntity = XMLNode::CreateDocumentNode(*this).entityHandle().entity();

  registry_->ctx().emplace<XMLNamespaceContext>(*registry_);
}

XMLNode XMLDocument::root() const {
  return XMLNode(rootEntityHandle());
}

EntityHandle XMLDocument::rootEntityHandle() const {
  return EntityHandle(*registry_, registry_->ctx().get<XMLDocumentContext>().rootEntity);
}

bool XMLDocument::operator==(const XMLDocument& other) const {
  return &registry_->ctx().get<const XMLDocumentContext>() ==
         &other.registry_->ctx().get<const XMLDocumentContext>();
}
}  // namespace donner::xml
