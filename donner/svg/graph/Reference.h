#pragma once
/// @file

#include "donner/base/RcString.h"
#include "donner/svg/registry/Registry.h"

namespace donner::svg {

struct ResolvedReference {
  EntityHandle handle;

  operator Entity() const { return handle.entity(); }
};

struct Reference {
  RcString href;

  /* implicit */ Reference(RcString href) : href(std::move(href)) {}
  /* implicit */ Reference(const char* href) : href(href) {}

  std::optional<ResolvedReference> resolve(Registry& registry) const;

  bool operator==(const Reference& other) const = default;
};

}  // namespace donner::svg
