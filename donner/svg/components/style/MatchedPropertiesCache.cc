#include "donner/svg/components/style/MatchedPropertiesCache.h"

#include <algorithm>
#include <bit>
#include <functional>
#include <utility>

namespace donner::svg::components {
namespace {

void Combine(std::size_t& hash, std::size_t value) {
  hash ^= value + 0x9e3779b9u + (hash << 6) + (hash >> 2);
}

void HashSpecificity(std::size_t& hash, css::Specificity specificity) {
  Combine(hash, static_cast<std::size_t>(specificity.specialValue()));
  Combine(hash, specificity.abc().a);
  Combine(hash, specificity.abc().b);
  Combine(hash, specificity.abc().c);
}

void HashOpacity(std::size_t& hash, const MatchedOpacityInput& opacity) {
  Combine(hash, static_cast<std::size_t>(opacity.state));
  if (opacity.value) {
    Combine(hash, std::hash<std::uint64_t>{}(std::bit_cast<std::uint64_t>(*opacity.value)));
  }
  HashSpecificity(hash, opacity.specificity);
}

std::optional<std::string> LocalKey(const StyleComponent* local) {
  if (!local || local->hasEmptyDeclarations()) {
    return std::string();
  }
  return local->properties().localDeclarationKey();
}

}  // namespace

bool MatchedOpacityInput::operator==(const MatchedOpacityInput& other) const {
  return state == other.state && specificity == other.specificity &&
         value.has_value() == other.value.has_value() &&
         (!value ||
          std::bit_cast<std::uint64_t>(*value) == std::bit_cast<std::uint64_t>(*other.value));
}

void MatchedPropertiesKey::addRule(std::uint64_t sheetRevision, std::size_t ordinal,
                                   std::uint32_t index, css::Specificity specificity) {
  if (rules_.size() >= kMaximumRules) {
    tooManyRules_ = true;
    return;
  }
  rules_.push_back({sheetRevision, ordinal, index, specificity});
  Combine(ruleHash_, std::hash<std::uint64_t>{}(sheetRevision));
  Combine(ruleHash_, ordinal);
  Combine(ruleHash_, index);
  HashSpecificity(ruleHash_, specificity);
}

bool MatchedPropertiesKey::cacheable() const {
  return !tooManyRules_ && (!localStyle || localStyle->declarationHash().has_value());
}

std::size_t MatchedPropertiesKey::hash() const {
  auto hash = ruleHash_;
  Combine(hash, std::hash<const ComputedStyle*>{}(parent.get()));
  Combine(hash, std::hash<const ComputedStyle*>{}(root.get()));
  Combine(hash, std::hash<std::optional<ElementType>>{}(elementType));
  Combine(hash, noInheritPaint);
  Combine(hash, instanceSymbol);
  Combine(hash, documentRoot);
  HashOpacity(hash, stop);
  HashOpacity(hash, flood);
  Combine(hash, localStyle ? *localStyle->declarationHash() : std::hash<std::string_view>{}(""));
  return hash;
}

bool MatchedPropertiesKey::sameContextAndRules(const MatchedPropertiesKey& other) const {
  return parent == other.parent && root == other.root && elementType == other.elementType &&
         noInheritPaint == other.noInheritPaint && instanceSymbol == other.instanceSymbol &&
         documentRoot == other.documentRoot && stop == other.stop && flood == other.flood &&
         ruleHash_ == other.ruleHash_ && std::ranges::equal(rules_, other.rules_);
}

const MatchedPropertiesEntry* MatchedPropertiesCache::find(const MatchedPropertiesKey& key) {
  if (!key.cacheable()) {
    return nullptr;
  }
  auto& bucket = buckets_[key.hash() % kBuckets];
  if (!bucket) {
    return nullptr;
  }
  const auto local = LocalKey(key.localStyle);
  for (auto& slot : bucket->slots) {
    if (slot.key.sameContextAndRules(key) && slot.localKey == local) {
      slot.referenced = true;
      return &slot.entry;
    }
  }
  return nullptr;
}

std::size_t MatchedPropertiesCache::Slot::keyPayloadBytes() const {
  // Count inline capacity here as well; overcounting keeps the bound independent of the STL.
  return (localKey.capacity() + 1) + key.rules_.capacity() * sizeof(MatchedPropertiesKey::Rule);
}

MatchedPropertiesCache::Slot& MatchedPropertiesCache::Bucket::nextVictim() {
  // At most one complete turn clears all reference bits before finding a victim.
  while (std::exchange(slots[hand].referenced, false)) {
    hand = (hand + 1) % kSlotsPerBucket;
  }
  return slots[hand];
}

void MatchedPropertiesCache::insert(const MatchedPropertiesKey& key, MatchedPropertiesEntry entry) {
  if (!key.cacheable()) {
    return;
  }
  auto local = LocalKey(key.localStyle);
  if (!local || local->capacity() > kMaximumStringCapacity) {
    return;
  }
  auto& bucket = buckets_[key.hash() % kBuckets];
  Slot incoming{key, std::move(*local), std::move(entry)};
  incoming.key.localStyle = nullptr;
  Slot* victim =
      bucket && bucket->slots.size() == kSlotsPerBucket ? &bucket->nextVictim() : nullptr;
  const std::size_t keptBytes = retainedKeyPayloadBytes_ - (victim ? victim->keyPayloadBytes() : 0);
  const std::size_t incomingBytes = incoming.keyPayloadBytes();
  if (incomingBytes > kMaximumKeyPayloadBytes - keptBytes) {
    return;
  }
  retainedKeyPayloadBytes_ = keptBytes + incomingBytes;

  if (victim) {
    // Reconstruct the slot so an old string allocation cannot survive a smaller key.
    std::destroy_at(victim);
    std::construct_at(victim, std::move(incoming));
    bucket->hand = (bucket->hand + 1) % kSlotsPerBucket;
  } else {
    if (!bucket) {
      bucket = std::make_unique<Bucket>();
    }
    bucket->slots.emplace_back(std::move(incoming));
  }
}

void MatchedPropertiesCache::clear() {
  for (auto& bucket : buckets_) {
    bucket.reset();
  }
  retainedKeyPayloadBytes_ = 0;
}

void MatchedPropertiesCache::discardComputedStyles(Registry& registry) {
  if (auto* cache = registry.ctx().find<MatchedPropertiesCache>()) {
    cache->clear();
  }
  registry.clear<ComputedStyleComponent>();
}

}  // namespace donner::svg::components
