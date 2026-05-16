#pragma once
/// @file

#include <memory>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Utils.h"

namespace donner::xml {

// Forward declarations
class XMLDocument;
class XMLNode;
class XMLSourceStore;

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
   */
  explicit XMLDocumentContext(InternalCtorTag ctorTag) {}

  /// Root entity of the document.
  Entity rootEntity = entt::null;

  /// Optional source store for parsed documents that own their source projection.
  std::shared_ptr<XMLSourceStore> sourceStore;
};

}  // namespace donner::xml::components
