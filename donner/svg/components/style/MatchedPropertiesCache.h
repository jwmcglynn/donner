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

  std::size_t hash() const { return hash(parent.get(), root.get()); }
  std::size_t hash(const ComputedStyle* parentRecord, const ComputedStyle* rootRecord) const;
  bool sameContextAndRules(const MatchedPropertiesKey& other, const ComputedStyle* parentRecord,
                           const ComputedStyle* rootRecord) const;
  static constexpr std::size_t kMaximumRules = 128;
  SmallVector<Rule, 4> rules_;
  std::size_t ruleHash_ = 0;
  // Revisions distinguish equality, but cannot move the clock bucket across identical parses.
  std::size_t placementRuleHash_ = 0;
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
  /// Worker fronts limit eviction metadata independently of the canonical document cache.
  enum class Scope { Document, Worker };
  explicit MatchedPropertiesCache(Scope scope = Scope::Document)
      : bucketCount_(scope == Scope::Worker ? kWorkerBuckets : kBuckets) {}

  /// Conservative reservation for all metadata that one worker front can retain.
  static constexpr std::size_t workerMetadataBytes() {
    return sizeof(MatchedPropertiesCache) + kWorkerBuckets * kWorkerBucketBytes;
  }

  /// Returns the record for an exactly equal key. Valid until the next mutation of this cache.
  const MatchedPropertiesEntry* find(const MatchedPropertiesKey& key) {
    return find(key, key.parent.get(), key.root.get());
  }
  /// Probe with borrowed records under document access. Insertion takes strong references.
  const MatchedPropertiesEntry* find(const MatchedPropertiesKey& key,
                                     const ComputedStyle* parentRecord,
                                     const ComputedStyle* rootRecord);
  /// Own an immutable snapshot of the inputs if the cache can retain the key.
  bool insert(const MatchedPropertiesKey& key, MatchedPropertiesEntry entry);
  /// Changes whenever a clock bit can be cleared or a stored record removed.
  std::uint64_t evictionEpoch() const { return evictionEpoch_; }

  /// Release cached keys and record references when the stylesheet generation changes.
  void clear();

  /// Discard all computed components and the keys from their stylesheet generation together.
  static void discardComputedStyles(Registry& registry);

private:
  friend struct MatchedPropertiesCacheTestPeer;
  bool accepts(const MatchedPropertiesKey& key) const;

  struct Slot {
    MatchedPropertiesKey key;
    std::string localKey;
    MatchedPropertiesEntry entry;
    bool referenced = true;

    std::size_t keyPayloadBytes() const;
  };

  struct Bucket {
    SmallVector<Slot, 1> slots;
    std::size_t hand = 0;

    Slot& nextVictim();
  };

  Slot* findVictim(Bucket* bucket);

  static constexpr std::size_t kSlotsPerBucket = 8;
  static constexpr std::size_t kBuckets = 4096 / kSlotsPerBucket;
  static constexpr std::size_t kMaximumStringCapacity = 4096;
  static constexpr std::size_t kMaximumKeyPayloadBytes = 1024 * 1024;
  // A bucket can retain its inline slot and an eight-slot heap allocation at the same time.
  static constexpr std::size_t kMaximumMetadataBytes =
      kBuckets *
          (sizeof(std::unique_ptr<Bucket>) + sizeof(Bucket) + kSlotsPerBucket * sizeof(Slot)) +
      kMaximumKeyPayloadBytes;
  static_assert(kMaximumMetadataBytes <= 3 * 1024 * 1024);
  static constexpr std::size_t kWorkerBuckets = 16;
  static constexpr std::size_t kWorkerLocalCapacity = 512;
  static constexpr std::size_t kWorkerRuleCapacity = 4;
  // Worker keys stay in their four inline rule slots.
  static constexpr std::size_t kWorkerBucketBytes =
      sizeof(Bucket) + kSlotsPerBucket * (sizeof(Slot) + kWorkerLocalCapacity + 1);
  std::size_t bucketCount_;
  std::uint64_t evictionEpoch_ = 0;
  std::array<std::unique_ptr<Bucket>, kBuckets> buckets_{};
  std::size_t retainedKeyPayloadBytes_ = 0;
};

}  // namespace donner::svg::components
