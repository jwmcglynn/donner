#pragma once
/// @file

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "donner/base/SmallVector.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleComponent.h"

namespace donner::svg::components {

/// Context switch for comparing the cached and uncached paths on the same document.
struct DisableMatchedPropertiesCacheForTesting {};

/// The resource-specific declaration stored outside the local property registry.
struct MatchedOpacityInput {
  PropertyState state = PropertyState::NotSet;
  std::optional<double> value;
  css::Specificity specificity;

  bool operator==(const MatchedOpacityInput& other) const;
};

/// Inputs to a computed record. The local component is borrowed only during lookup or insertion.
struct MatchedPropertiesKey {
  /// Record one matched rule, in application order, after origin adjustment.
  void addRule(std::uint64_t sheetRevision, std::size_t ordinal, std::uint32_t index,
               css::Specificity specificity);
  /// Oversize keys use the ordinary cascade.
  [[nodiscard]] bool cacheable() const;

  std::shared_ptr<const ComputedStyle> parent;
  std::shared_ptr<const ComputedStyle> root;
  const StyleComponent* localStyle = nullptr;
  std::optional<ElementType> elementType;
  bool noInheritPaint = false;
  bool instanceSymbol = false;
  bool documentRoot = false;
  MatchedOpacityInput stop;
  MatchedOpacityInput flood;

private:
  friend class MatchedPropertiesCache;
  friend struct MatchedPropertiesCacheTestPeer;
  struct Rule {
    std::uint64_t sheetRevision;
    std::size_t ordinal;
    std::uint32_t index;
    css::Specificity specificity;

    bool operator==(const Rule&) const = default;
  };

  std::size_t hash() const;
  bool sameContextAndRules(const MatchedPropertiesKey& other) const;
  static constexpr std::size_t kMaximumRules = 128;
  SmallVector<Rule, 4> rules_;
  std::size_t ruleHash_ = 0;
  bool tooManyRules_ = false;
};

/// Record and paint-origin inputs needed to install one occurrence of a shared style.
struct MatchedPropertiesEntry {
  std::shared_ptr<const ComputedStyle> style;
  PropertyState fillState = PropertyState::NotSet;
  PropertyState strokeState = PropertyState::NotSet;
};

/// Document-local second-chance cache. Each bucket has its own clock and at most eight entries.
class MatchedPropertiesCache {
public:
  /// Returns the record for an exactly equal key. Valid until the next mutation of this cache.
  const MatchedPropertiesEntry* find(const MatchedPropertiesKey& key);
  /// Own an immutable snapshot of the inputs; the source element can subsequently change or die.
  void insert(const MatchedPropertiesKey& key, MatchedPropertiesEntry entry);
  /// Release cached keys and record references when the stylesheet generation changes.
  void clear();

  /// Discard all computed components and the keys from their stylesheet generation together.
  static void discardComputedStyles(Registry& registry);

private:
  friend struct MatchedPropertiesCacheTestPeer;
  struct Slot {
    MatchedPropertiesKey key;
    std::string localKey;
    MatchedPropertiesEntry entry;
    bool referenced = true;
  };

  struct Bucket {
    SmallVector<Slot, 1> slots;
    std::size_t hand = 0;
  };

  static constexpr std::size_t kSlotsPerBucket = 8;
  static constexpr std::size_t kBuckets = 4096 / kSlotsPerBucket;
  // Bound retained key storage, including container capacity, separately from the records.
  // The embedded slot in each bucket is counted even while that bucket uses its heap buffer.
  static constexpr std::size_t kMaximumStringCapacity = 4096;
  static constexpr std::size_t kMaximumMetadataBytes =
      kBuckets * (sizeof(std::unique_ptr<Bucket>) + sizeof(Bucket) +
                  kSlotsPerBucket *
                      (sizeof(Slot) + kMaximumStringCapacity + 1 +
                       MatchedPropertiesKey::kMaximumRules * sizeof(MatchedPropertiesKey::Rule)));
  static_assert(kMaximumMetadataBytes <= 40 * 1024 * 1024);
  std::array<std::unique_ptr<Bucket>, kBuckets> buckets_{};
};

}  // namespace donner::svg::components
