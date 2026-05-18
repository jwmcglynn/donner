#pragma once
/// @file

#include <cstdint>

#include "donner/base/EcsRegistry.h"
#include "donner/base/RcString.h"
#include "donner/base/Utils.h"
#include "donner/base/Vector2.h"
#include "donner/svg/SVGDocumentHandle.h"
#include "donner/svg/components/IdComponent.h"

namespace donner::svg {

// Forward declarations
class ElementAnchor;
class SVGDocument;
class SVGElement;

}  // namespace donner::svg

/**
 * Contains the implementation of the Donner ECS, \see \ref EcsArchitecture.
 *
 * Classes are named as follows:
 * - `*System` - Stateless object that accesses and manipulates components.
 * - `*Component` - Data storage struct with minimal logic, which may be attached to an \ref Entity.
 * - `Computed*Component` - Created when processing the tree and preparing it for rendering, stores
 * intermediate object state derived from regular components. Is used to apply the CSS cascade and
 * to cache shape path data.
 * - `*Context` - Contains global state of an SVG document, which are stored in the \c
 * Registry::ctx().
 * - `*Tag` - Used to tag an entity with a specific purpose, like to disable rendering behavior
 * (like inheritance with \ref DoNotInheritFillOrStrokeTag).
 */
namespace donner::svg::components {

class TreeMutation;

/**
 * Holds global state of an SVG document, such as the root element, id-to-element mapping, and the
 * document size.
 *
 * One instance of this class is created per SVG document.
 *
 * Access the document context via the \c Registry::ctx API:
 * ```
 * SVGDocumentContext& context = registry.ctx().get<SVGDocumentContext>();
 * Entity foo = context.getEntityById("foo");
 * ```
 */
class SVGDocumentContext {
private:
  friend class ::donner::svg::ElementAnchor;
  friend class ::donner::svg::SVGDocument;
  friend class ::donner::svg::SVGElement;

  /// Tag to allow internal construction, used by \ref SVGDocument.
  struct InternalCtorTag {};

public:
  /**
   * Internal constructor, creates a context on the given \ref SVGDocument.
   *
   * To use this class, access it via the \c Registry::ctx API.
   * ```
   * SVGDocumentContext& context = registry.ctx().get<SVGDocumentContext>();
   * ```
   *
   * @param ctorTag Internal tag to allow construction.
   * @param documentState Shared state for the document.
   */
  explicit SVGDocumentContext(InternalCtorTag ctorTag, SVGDocumentHandle documentState);

  /// Current canvas size, if set. Equivalent to the window size, which controls how the SVG
  /// contents are rendered.
  std::optional<Vector2i> canvasSize;

  /// Root entity of the document, which contains the \ref xml_svg element.
  Entity rootEntity = entt::null;

  /**
   * Get the entity with the given ID, using the internal id-to-entity mapping.
   *
   * If multiple elements have the same id, the first one that was created will be returned.
   *
   * @param id ID to find the entity for.
   */
  Entity getEntityById(const RcString& id) const {
    const auto it = idToEntity_.find(id);
    return (it != idToEntity_.end()) ? it->second : entt::null;
  }

  /// Mark the backing document state as mutated.
  void bumpMutationRevision() const { getSharedDocumentState()->bumpMutationRevision(); }

  /// Returns true if the current thread holds write access to the backing document.
  bool currentThreadHasWriteAccess() const {
    return getSharedDocumentState()->currentThreadHasWriteAccess();
  }

  /// Acquire write access to the backing document.
  DocumentWriteAccess writeAccess() const { return getSharedDocumentState()->write(); }

  /// Get detached-node collection state for the backing document.
  DetachedNodeState& detachedNodeState() const {
    return getSharedDocumentState()->detachedNodeState();
  }

  /// Returns true when detached-node collection is deferred for a snapshot or observer epoch.
  bool hasActiveDetachedNodeCollectionDeferral() const {
    return getSharedDocumentState()->hasActiveDetachedNodeCollectionDeferral();
  }

  /// Current detached-node collection epoch high-water.
  std::uint64_t activeDetachedNodeCollectionEpoch() const {
    return getSharedDocumentState()->activeDetachedNodeCollectionEpoch();
  }

private:
  /// Rehydrate the shared document state. Asserts if it has already been destroyed, which means
  /// that this object is likely invalid too.
  SVGDocumentHandle getSharedDocumentState() const {
    if (auto documentState = documentState_.lock()) {
      return documentState;
    } else {
      UTILS_RELEASE_ASSERT_MSG(false, "SVGDocument has already been destroyed");
    }
  }

  /// Rehydrate the shared_ptr for the Registry.
  std::shared_ptr<Registry> getSharedRegistry() const {
    return getSharedDocumentState()->sharedRegistry();
  }

  /// Called when an ID is added to an element.
  void onIdSet(Registry& registry, Entity entity) {
    auto& idComponent = registry.get<IdComponent>(entity);
    idToEntity_.emplace(idComponent.id(), entity);
  }

  /// Called when an ID is removed from an element.
  void onIdDestroy(Registry& registry, Entity entity) {
    auto& idComponent = registry.get<IdComponent>(entity);
    idToEntity_.erase(idComponent.id());
  }

  /// Shared document state reference, owned by SVGDocument and retained public DOM handles.
  std::weak_ptr<DocumentState> documentState_;

  /// Mapping from ID to entity.
  std::unordered_map<RcString, Entity> idToEntity_;
};

}  // namespace donner::svg::components
