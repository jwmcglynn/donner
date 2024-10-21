#pragma once
/// @file

#include "donner/base/EcsRegistry.h"
#include "donner/base/Utils.h"

namespace donner::xml {

// Forward declarations
class XMLDocument;
class XMLNode;

}  // namespace donner::xml

namespace donner::xml::components {

/**
 * Holds global state of an XML document, such as the root element.
 *
 * One instance of this class is created per XML document.
 *
 * Access the document context via the \c Registry::ctx API:
 * ```
 * XMLDocumentContext& context = registry.ctx().get<XMLDocumentContext>();
 * ```
 */
class XMLDocumentContext {
private:
  friend class donner::xml::XMLDocument;
  friend class donner::xml::XMLNode;

  /// Tag to allow internal construction, used by \ref XMLDocument.
  struct InternalCtorTag {};

public:
  /**
   * Internal constructor, creates a context on the given \ref XMLDocument.
   *
   * To use this class, access it via the \c Registry::ctx API.
   * ```
   * XMLDocumentContext& context = registry.ctx().get<XMLDocumentContext>();
   * ```
   *
   * @param ctorTag Internal tag to allow construction.
   * @param registry Underlying registry for the document.
   */
  explicit XMLDocumentContext(InternalCtorTag ctorTag, const std::shared_ptr<Registry>& registry)
      : registry_(registry) {}

  /// Root entity of the document.
  Entity rootEntity = entt::null;

private:
  /// Rehydrate the shared_ptr for the Registry. Asserts if the registry has already been destroyed,
  /// which means that this object is likely invalid too.
  std::shared_ptr<Registry> getSharedRegistry() const {
    if (auto registry = registry_.lock()) {
      return registry;
    } else {
      UTILS_RELEASE_ASSERT_MSG(false, "XMLDocument has already been destroyed");
    }
  }

  /// ECS registry reference, which is owned by XMLDocument. This is used to recreate an
  /// XMLDocument when requested, and will fail if all references have been destroyed.
  std::weak_ptr<Registry> registry_;
};

}  // namespace donner::xml::components
