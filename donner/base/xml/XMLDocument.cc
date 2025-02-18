#include "donner/base/xml/XMLDocument.h"

#include "donner/base/xml/components/XMLDocumentContext.h"
#include "donner/base/xml/components/XMLNamespaceContext.h"

namespace donner::xml {

using components::XMLDocumentContext;
using components::XMLNamespaceContext;

XMLDocument::XMLDocument() : registry_(std::make_shared<Registry>()) {
  auto& ctx = registry_->ctx().emplace<XMLDocumentContext>(XMLDocumentContext::InternalCtorTag{});
  ctx.rootEntity = XMLNode::CreateDocumentNode(*this).entityHandle().entity();

  registry_->ctx().emplace<XMLNamespaceContext>(*registry_);
}

XMLNode XMLDocument::root() const {
  return XMLNode(rootEntityHandle());
}

EntityHandle XMLDocument::rootEntityHandle() const {
  return EntityHandle(*registry_, registry_->ctx().get<XMLDocumentContext>().rootEntity);
}

}  // namespace donner::xml
