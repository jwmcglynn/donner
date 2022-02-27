#pragma once

#include <cassert>
#include <set>

#include "src/svg/registry/registry.h"

namespace donner::svg {

struct RecursionGuard {
  RecursionGuard() = default;

  bool hasRecursion(Entity entity) const { return (entities_.find(entity) != entities_.end()); }

  void add(Entity entity) {
    [[maybe_unused]] const auto result = entities_.insert(entity);
    assert(result.second && "New element must be inserted");
  }

  RecursionGuard with(Entity entity) const {
    RecursionGuard result = *this;
    result.add(entity);
    return result;
  }

private:
  std::set<Entity> entities_;
};

}  // namespace donner::svg
