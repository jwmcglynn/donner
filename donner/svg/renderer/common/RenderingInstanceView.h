#pragma once
/// @file

#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/registry/Registry.h"

namespace donner::svg {

struct RenderingInstanceView {
public:
  using StorageType = entt::storage<components::RenderingInstanceComponent>;
  using Iterator = StorageType::const_iterator;

  explicit RenderingInstanceView(Registry& registry)
      : storage_(registry.storage<components::RenderingInstanceComponent>()),
        current_(storage_.begin()),
        end_(storage_.end()) {}

  bool done() const { return current_ == end_; }
  void advance() {
    assert(!done());
    ++current_;
  }

  Entity currentEntity() const {
    assert(!done());
    return entt::to_entity(storage_, *current_);
  }

  const components::RenderingInstanceComponent& get() const { return *current_; }

private:
  StorageType& storage_;
  Iterator current_;
  Iterator end_;
};

}  // namespace donner::svg
