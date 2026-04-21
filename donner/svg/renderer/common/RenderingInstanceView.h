#pragma once
/// @file

#include <span>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/RenderingInstanceComponent.h"

namespace donner::svg {

/// A view containing a list of \ref components::RenderingInstanceComponent which can be iterated
/// over.
///
/// The view snapshots the entity IDs it will visit at construction time. This lets the caller
/// safely mutate the underlying \ref components::RenderingInstanceComponent storage during
/// iteration — for example, a filter-graph pre-pass that calls
/// `RenderingContext::createFeImageShadowTree`, which emplaces new rendering instances and sorts
/// the whole pool. Iterating over live entt iterators across such mutation would be undefined
/// behaviour; iterating a snapshot of entity IDs is stable, and each component access re-reads
/// from storage using `storage.get(entity)`.
struct RenderingInstanceView {
public:
  /// The type of the storage used to store the components.
  using StorageType = entt::storage<components::RenderingInstanceComponent>;

  /**
   * Constructor, takes a registry and snapshots every entity that currently has a \ref
   * components::RenderingInstanceComponent, in storage iteration order.
   *
   * @param registry The registry to use.
   */
  explicit RenderingInstanceView(Registry& registry)
      : storage_(registry.storage<components::RenderingInstanceComponent>()) {
    entities_.reserve(storage_.size());
    for (auto it = storage_.begin(), end = storage_.end(); it != end; ++it) {
      entities_.push_back(entt::to_entity(storage_, *it));
    }
    end_ = entities_.size();
  }

  /**
   * Constructor taking an explicit ordered list of entities to iterate. The view copies the list;
   * subsequent storage mutations do not affect iteration. Entities must all currently have a \ref
   * components::RenderingInstanceComponent — `get()` will dereference via storage lookup.
   *
   * @param registry The registry to use.
   * @param entities Ordered entity list to iterate.
   */
  RenderingInstanceView(Registry& registry, std::span<const Entity> entities)
      : storage_(registry.storage<components::RenderingInstanceComponent>()),
        entities_(entities.begin(), entities.end()),
        end_(entities_.size()) {}

  /// Destructor.
  ~RenderingInstanceView() = default;

  // Moveable and copyable on construction only.
  /// Copy constructor.
  RenderingInstanceView(const RenderingInstanceView&) = default;
  /// Move constructor.
  RenderingInstanceView(RenderingInstanceView&&) = default;
  RenderingInstanceView& operator=(const RenderingInstanceView&) = delete;
  RenderingInstanceView& operator=(RenderingInstanceView&&) = delete;

  /// Returns true if the view has no more elements.
  bool done() const { return current_ == end_; }

  /**
   * Advances the view to the next element.
   *
   * @pre List has elements remaining \ref done() is `false`.
   */
  void advance() {
    assert(!done());
    ++current_;
  }

  /// Returns the current entity.
  Entity currentEntity() const {
    assert(!done());
    return entities_[current_];
  }

  /// Opaque iterator state returned by \ref save and consumed by \ref restore.
  struct SavedState {
    size_t current;  ///< Saved index into the snapshot entity list.
  };

  /// Saves the current state.
  SavedState save() const { return {current_}; }

  /// Restores the state.
  void restore(const SavedState& state) { current_ = state.current; }

  /// Returns the current component.
  const components::RenderingInstanceComponent& get() const {
    assert(!done());
    return storage_.get(entities_[current_]);
  }

private:
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
  StorageType& storage_;          //!< The storage containing the components.
  std::vector<Entity> entities_;  //!< Snapshot of entity IDs in iteration order.
  size_t current_ = 0;            //!< Index into \ref entities_ of the current element.
  size_t end_ = 0;                //!< One past the last valid index.
};

}  // namespace donner::svg
