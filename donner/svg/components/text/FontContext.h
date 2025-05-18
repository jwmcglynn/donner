#pragma once
/// @file

#include <unordered_map>

#include "donner/base/EcsRegistry.h"
#include "donner/base/RcString.h"
#include "include/core/SkData.h"
#include "include/core/SkTypeface.h"

namespace donner::svg::components {

class FontContext {
public:
  explicit FontContext(Registry& registry);

  void addFont(const RcString& family, sk_sp<SkData> data);
  sk_sp<SkTypeface> getTypeface(const RcString& family) const;
  Registry& registry() const { return registry_; }

private:
  Registry& registry_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
  std::unordered_map<RcString, sk_sp<SkTypeface>> fonts_;
};

}  // namespace donner::svg::components
