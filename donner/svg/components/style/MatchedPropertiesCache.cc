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

void MatchedPropertiesKey::addRule(Entity sheet, std::size_t ordinal, std::uint32_t index,
                                   css::Specificity specificity) {
  if (rules_.size() >= kMaximumRules) {
    tooManyRules_ = true;
    return;
  }
  rules_.push_back({sheet, ordinal, index, specificity});
  Combine(ruleHash_, static_cast<std::size_t>(sheet));
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

void MatchedPropertiesCache::insert(const MatchedPropertiesKey& key, MatchedPropertiesEntry entry) {
  if (!key.cacheable()) {
    return;
  }
  auto local = LocalKey(key.localStyle);
  if (!local || local->capacity() > kMaximumStringCapacity) {
    return;
  }
  auto& bucket = buckets_[key.hash() % kBuckets];
  if (!bucket) {
    bucket = std::make_unique<Bucket>();
  }
  Slot incoming{key, std::move(*local), std::move(entry)};
  incoming.key.localStyle = nullptr;
  if (bucket->slots.size() < kSlotsPerBucket) {
    bucket->slots.emplace_back(std::move(incoming));
    return;
  }

  // At most one complete turn clears all the reference bits. The next probe finds a victim.
  while (std::exchange(bucket->slots[bucket->hand].referenced, false)) {
    bucket->hand = (bucket->hand + 1) % kSlotsPerBucket;
  }
  bucket->slots[bucket->hand] = std::move(incoming);
  bucket->hand = (bucket->hand + 1) % kSlotsPerBucket;
}

void MatchedPropertiesCache::clear() {
  for (auto& bucket : buckets_) {
    bucket.reset();
  }
}

void MatchedPropertiesCache::discardComputedStyles(Registry& registry) {
  if (auto* cache = registry.ctx().find<MatchedPropertiesCache>()) {
    cache->clear();
  }
  registry.clear<ComputedStyleComponent>();
}

}  // namespace donner::svg::components
