#pragma once
/// @file

#include "src/svg/components/rendering_instance_component.h"
#include "src/svg/registry/registry.h"

namespace donner::svg {

struct RenderingInstanceView {
public:
  using ViewType = entt::basic_view<Entity, entt::get_t<components::RenderingInstanceComponent>,
                                    entt::exclude_t<>, void>;
  using Iterator = ViewType::iterator;

  explicit RenderingInstanceView(const ViewType& view)
      : view_(view), current_(view.begin()), end_(view.end()) {}

  bool done() const { return current_ == end_; }
  void advance() { ++current_; }

  Entity currentEntity() const {
    assert(!done());
    return *current_;
  }

  const components::RenderingInstanceComponent& get() const {
    return view_.get<components::RenderingInstanceComponent>(currentEntity());
  }

private:
  const ViewType& view_;
  Iterator current_;
  Iterator end_;
};

}  // namespace donner::svg
