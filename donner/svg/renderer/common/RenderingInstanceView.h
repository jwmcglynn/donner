#pragma once
/// @file

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/RenderingInstanceComponent.h"

namespace donner::svg {

/// A view containing a list of \ref components::RenderingInstanceComponent which can be iterated
/// over.
struct RenderingInstanceView {
public:
  /// The type of the storage used to store the components.
  using StorageType = entt::storage<components::RenderingInstanceComponent>;
  /// Iterator type.
  using Iterator = StorageType::const_iterator;

  /**
   * Constructor, takes a registry and creates a view over all \ref
   * components::RenderingInstanceComponent.
   *
   * @param registry The registry to use.
   */
  explicit RenderingInstanceView(Registry& registry)
      : storage_(registry.storage<components::RenderingInstanceComponent>()),
        current_(storage_.begin()),
        end_(storage_.end()) {}

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
    return entt::to_entity(storage_, *current_);
  }

  struct SavedState {
    Iterator current;
  };

  /// Saves the current state.
  SavedState save() const { return {current_}; }

  /// Restores the state.
  void restore(const SavedState& state) { current_ = state.current; }

  /// Returns the current component.
  const components::RenderingInstanceComponent& get() const { return *current_; }

private:
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
  StorageType& storage_;  //!< The storage containing the components.
  Iterator current_;      //!< The current iterator.
  Iterator end_;          //!< The end iterator.
};

}  // namespace donner::svg
