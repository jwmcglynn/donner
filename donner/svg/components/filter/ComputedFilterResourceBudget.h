#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"

namespace donner::svg::components {

/** Compile seam for computed-filter accounting; enforcement is added by the green commit. */
class ComputedFilterResourceBudget {
public:
  static constexpr std::size_t kMaximumBytes = 16 * 1024 * 1024;
  struct Limits {
    std::size_t maximumBytes = kMaximumBytes;
  };

  explicit ComputedFilterResourceBudget(
      std::shared_ptr<DocumentResourceFamilyBudget> family = nullptr)
      : family_(std::move(family)) {}
  ComputedFilterResourceBudget(std::shared_ptr<DocumentResourceFamilyBudget> family, Limits limits)
      : family_(std::move(family)), limits_(limits) {}

  bool reserve(Entity, std::size_t) { return true; }
  void release(Entity) {}

  std::shared_ptr<const std::vector<std::uint8_t>> shareImage(
      Entity, const std::vector<std::uint8_t>& sourcePixels) {
    ++sharedImageMaterializations_;
    return std::make_shared<const std::vector<std::uint8_t>>(sourcePixels);
  }

  std::size_t retainedBytes() const { return 0; }
  std::size_t sharedImageMaterializations() const { return sharedImageMaterializations_; }
  bool rejected() const { return false; }
  const Limits& limits() const { return limits_; }

private:
  std::shared_ptr<DocumentResourceFamilyBudget> family_;
  Limits limits_;
  std::size_t sharedImageMaterializations_ = 0;
};

}  // namespace donner::svg::components
