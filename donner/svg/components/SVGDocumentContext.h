#pragma once
/// @file

#include "donner/base/EcsRegistry.h"
#include "donner/base/RcString.h"
#include "donner/base/Utils.h"
#include "donner/base/Vector2.h"
#include "donner/svg/components/IdComponent.h"

namespace donner::svg {

// Forward declarations
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
   * @param registry Underlying registry for the document.
   */
  explicit SVGDocumentContext(InternalCtorTag ctorTag, const std::shared_ptr<Registry>& registry);

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

private:
  /// Rehydrate the shared_ptr for the Registry. Asserts if the registry has already been destroyed,
  /// which means that this object is likely invalid too.
  std::shared_ptr<Registry> getSharedRegistry() const {
    if (auto registry = registry_.lock()) {
      return registry;
    } else {
      UTILS_RELEASE_ASSERT_MSG(false, "SVGDocument has already been destroyed");
    }
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

  /// ECS registry reference, which is owned by SVGDocument. This is used to recreate an
  /// SVGDocument when requested, and will fail if all references have been destroyed.
  std::weak_ptr<Registry> registry_;

  /// Mapping from ID to entity.
  std::unordered_map<RcString, Entity> idToEntity_;
};

}  // namespace donner::svg::components
