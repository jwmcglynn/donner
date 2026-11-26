#include "donner/svg/components/style/RuleIndex.h"

#include <algorithm>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <variant>

#include "donner/svg/components/style/SelectorWalk.h"

namespace donner::svg::components {
namespace {

using Kind = RuleIndexKey::Kind;

/// One selector-list entry filed under a key, before buckets are formed.
struct FiledEntry {
  RuleIndexKey key;
  std::uint32_t rule;
};

bool HasNoNamespace(const css::AttributeSelector& selector) {
  return selector.name.name.namespacePrefix.empty();
}

bool IsNamedType(const css::TypeSelector& selector) {
  return !selector.isUniversal();
}

/// The compounds of \p selector reached from its subject through descendant and child
/// combinators, nearest first, stopping at the first sibling combinator.
SmallVector<const css::CompoundSelector*, 4> AncestorCompounds(
    const css::ComplexSelector& selector) {
  SmallVector<const css::CompoundSelector*, 4> compounds;
  for (std::size_t i = selector.entries.size(); i > 1; --i) {
    const css::Combinator combinator = selector.entries[i - 1].combinator;
    if (combinator != css::Combinator::Descendant && combinator != css::Combinator::Child) {
      break;
    }
    compounds.push_back(&selector.entries[i - 2].compoundSelector);
  }
  return compounds;
}

/// The ancestor-filter key \p simple gives, if any: an id, a class, an attribute local name
/// without a namespace or a non-universal type name, lowercased where matching ignores case.
std::optional<RuleIndexKey> AncestorKeyOf(const css::CompoundSelector::Entry& simple) {
  if (const auto* id = std::get_if<css::IdSelector>(&simple)) {
    return RuleIndexKey{.kind = Kind::Id, .name = id->name};
  }
  if (const auto* className = std::get_if<css::ClassSelector>(&simple)) {
    return RuleIndexKey{.kind = Kind::Class, .name = className->name};
  }
  if (const auto* attribute = std::get_if<css::AttributeSelector>(&simple);
      attribute != nullptr && HasNoNamespace(*attribute)) {
    return RuleIndexKey{.kind = Kind::Attribute, .name = Lowercased(attribute->name.name.name)};
  }
  if (const auto* type = std::get_if<css::TypeSelector>(&simple);
      type != nullptr && IsNamedType(*type)) {
    return RuleIndexKey{.kind = Kind::Type, .name = Lowercased(type->matcher.name)};
  }
  return std::nullopt;
}

/// Every selector-list entry of \p rules with its key, sorted by kind, key and rule, without
/// repeats: a rule listed twice under one key, as in `.a, .a > b`, is a candidate once.
std::vector<FiledEntry> FileEntries(std::span<const css::SelectorRule> rules) {
  std::vector<FiledEntry> filed;
  for (std::size_t rule = 0; rule < rules.size(); ++rule) {
    for (const css::ComplexSelector& selector : rules[rule].selector.entries) {
      filed.push_back(FiledEntry{RuleIndex::KeyFor(selector), static_cast<std::uint32_t>(rule)});
    }
  }
  const auto order = [](const FiledEntry& entry) {
    return std::make_tuple(entry.key.kind, std::string_view(entry.key.name), entry.rule);
  };
  std::sort(filed.begin(), filed.end(),
            [&](const FiledEntry& lhs, const FiledEntry& rhs) { return order(lhs) < order(rhs); });
  filed.erase(std::unique(filed.begin(), filed.end(),
                          [&](const FiledEntry& lhs, const FiledEntry& rhs) {
                            return order(lhs) == order(rhs);
                          }),
              filed.end());
  return filed;
}

// These caps bound recursive matcher, specificity, and CSS component-value copy frames
// on a scheduler task. The eligibility walk itself also stops at the cap.
constexpr unsigned kMaximumParallelCssNesting = 4;

bool HasShallowSelectors(const css::Selector& selector, unsigned remaining) {
  for (const auto& compoundChain : selector.entries) {
    for (const auto& compound : compoundChain.entries) {
      for (const auto& entry : compound.compoundSelector.entries) {
        const auto* pseudo = std::get_if<css::PseudoClassSelector>(&entry);
        if (pseudo && pseudo->selector &&
            (remaining == 0 || !HasShallowSelectors(*pseudo->selector, remaining - 1))) {
          return false;
        }
      }
    }
  }
  return true;
}

bool HasShallowValues(std::span<const css::ComponentValue> values, unsigned remaining) {
  for (const auto& value : values) {
    std::span<const css::ComponentValue> children;
    if (const auto* fn = std::get_if<css::Function>(&value.value)) {
      children = fn->values;
    } else if (const auto* block = std::get_if<css::SimpleBlock>(&value.value)) {
      children = block->values;
    } else {
      continue;
    }
    if (remaining == 0 || !HasShallowValues(children, remaining - 1)) {
      return false;
    }
  }
  return true;
}

bool HasShallowRule(const css::SelectorRule& rule) {
  return HasShallowSelectors(rule.selector, kMaximumParallelCssNesting) &&
         std::ranges::all_of(rule.declarations, &RuleIndex::IsDeclarationShallowForParallelCascade);
}

}  // namespace

bool RuleIndex::IsDeclarationShallowForParallelCascade(const css::Declaration& declaration) {
  return HasShallowValues(declaration.values, kMaximumParallelCssNesting);
}

std::ostream& operator<<(std::ostream& os, const RuleIndexKey& key) {
  switch (key.kind) {
    case Kind::Id: return os << "Id(" << key.name << ")";
    case Kind::Class: return os << "Class(" << key.name << ")";
    case Kind::Attribute: return os << "Attribute(" << key.name << ")";
    case Kind::Type: return os << "Type(" << key.name << ")";
    case Kind::Universal: return os << "Universal";
  }
  return os;
}

// The 64-bit bound counts a 4-byte rule position, a 40-byte key record, a 16-byte ancestor hash
// group, a 12-byte specificity, and up to two per-rule offsets of 4 and 8 bytes.
static_assert(sizeof(void*) != 8 || RuleIndex::kMaxBytesPerEntry == 84,
              "Update the rule index memory bound in the CSS cascade design");

RuleIndexKey RuleIndex::KeyFor(const css::ComplexSelector& selector) {
  if (selector.entries.empty()) {
    return RuleIndexKey{};
  }
  const std::vector<css::CompoundSelector::Entry>& simple =
      selector.entries.back().compoundSelector.entries;
  if (const auto* id = FindSimple<css::IdSelector>(simple, kAnySelector)) {
    return RuleIndexKey{.kind = Kind::Id, .name = id->name};
  }
  if (const auto* className = FindSimple<css::ClassSelector>(simple, kAnySelector)) {
    return RuleIndexKey{.kind = Kind::Class, .name = className->name};
  }
  if (const auto* attribute = FindSimple<css::AttributeSelector>(simple, HasNoNamespace)) {
    return RuleIndexKey{.kind = Kind::Attribute, .name = Lowercased(attribute->name.name.name)};
  }
  if (const auto* type = FindSimple<css::TypeSelector>(simple, IsNamedType)) {
    return RuleIndexKey{.kind = Kind::Type, .name = Lowercased(type->matcher.name)};
  }
  return RuleIndexKey{};
}

SmallVector<std::uint32_t, 4> RuleIndex::AncestorHashesFor(const css::ComplexSelector& selector) {
  constexpr std::size_t kMaximumHashes = std::tuple_size_v<AncestorHashGroup>;
  const SmallVector<const css::CompoundSelector*, 4> compounds = AncestorCompounds(selector);
  SmallVector<std::uint32_t, 4> hashes;
  // Rarer kinds first: an id or class absent from the ancestors rejects more candidates.
  for (const Kind kind : {Kind::Id, Kind::Class, Kind::Attribute, Kind::Type}) {
    for (const css::CompoundSelector* compound : compounds) {
      for (const css::CompoundSelector::Entry& simple : compound->entries) {
        const std::optional<RuleIndexKey> key = AncestorKeyOf(simple);
        if (!key || key->kind != kind) {
          continue;
        }
        const std::uint32_t hash = AncestorKeyHash(kind, key->name).value();
        if (std::find(hashes.begin(), hashes.end(), hash) == hashes.end()) {
          hashes.push_back(hash);
          if (hashes.size() == kMaximumHashes) {
            return hashes;
          }
        }
      }
    }
  }
  return hashes;
}

void RuleIndex::addAncestorHashes(std::span<const css::SelectorRule> rules) {
  if (rules.empty()) {
    return;
  }
  hashGroupFirst_.reserve(rules.size() + 1);
  hashGroupFirst_.push_back(0);
  for (const css::SelectorRule& rule : rules) {
    const std::size_t first = hashGroups_.size();
    for (const css::ComplexSelector& entry : rule.selector.entries) {
      const SmallVector<std::uint32_t, 4> hashes = AncestorHashesFor(entry);
      if (hashes.size() == 0) {
        // This entry can match whatever the ancestors are, so the rule is never skipped.
        hashGroups_.resize(first);
        break;
      }
      AncestorHashGroup group;
      for (std::size_t i = 0; i < group.size(); ++i) {
        // A repeated hash is absent exactly when the original is, so padding changes nothing.
        group[i] = hashes[i < hashes.size() ? i : 0];
      }
      hashGroups_.push_back(group);
    }
    hashGroupFirst_.push_back(static_cast<std::uint32_t>(hashGroups_.size()));
  }
}

void RuleIndex::addSpecificities(std::span<const css::SelectorRule> rules) {
  specificityFirst_.reserve(rules.size());
  for (const css::SelectorRule& rule : rules) {
    specificityFirst_.push_back(specificities_.size());
    for (const auto& entry : rule.selector.entries) {
      specificities_.push_back(entry.computeSpecificity());
    }
  }
  specificities_.shrink_to_fit();
}

RuleIndex::RuleIndex(std::span<const css::SelectorRule> rules) : ruleCount_(rules.size()) {
  shallowForParallelCascade_ = std::ranges::all_of(rules, HasShallowRule);
  const std::vector<FiledEntry> filed = FileEntries(rules);
  entries_.reserve(filed.size());
  std::size_t keyBytes = 0;
  for (const FiledEntry& entry : filed) {
    keyBytes += addEntry(entry.key, entry.rule);
  }

  std::size_t bucketCount = 0;
  for (std::vector<Bucket>* buckets : {&ids_, &classes_, &attributes_, &types_}) {
    buckets->shrink_to_fit();
    bucketCount += buckets->size();
  }
  compactHitsAt_ = 2 * bucketCount + BucketHits().capacity();
  entries_.shrink_to_fit();
  addAncestorHashes(rules);
  addSpecificities(rules);
  hashGroups_.shrink_to_fit();
  hashGroupFirst_.shrink_to_fit();
  retainedBytes_ =
      entries_.capacity() * sizeof(std::uint32_t) +
      (ids_.capacity() + classes_.capacity() + attributes_.capacity() + types_.capacity()) *
          sizeof(Bucket) +
      hashGroups_.capacity() * sizeof(AncestorHashGroup) +
      hashGroupFirst_.capacity() * sizeof(std::uint32_t) +
      specificities_.capacity() * sizeof(css::Specificity::ABC) +
      specificityFirst_.capacity() * sizeof(std::size_t) + keyBytes;
}

std::vector<RuleIndex::Bucket>& RuleIndex::bucketsFor(RuleIndexKey::Kind kind) {
  switch (kind) {
    case Kind::Id: return ids_;
    case Kind::Class: return classes_;
    case Kind::Type: return types_;
    case Kind::Attribute:
    case Kind::Universal: break;
  }
  return attributes_;
}

std::size_t RuleIndex::addEntry(const RuleIndexKey& key, std::uint32_t rule) {
  const auto position = static_cast<std::uint32_t>(entries_.size());
  entries_.push_back(rule);
  if (key.kind == Kind::Universal) {
    universalFirst_ = universalCount_ == 0 ? position : universalFirst_;
    ++universalCount_;
    return 0;
  }
  std::vector<Bucket>& buckets = bucketsFor(key.kind);
  std::size_t keyBytes = 0;
  if (buckets.empty() || buckets.back().key != key.name) {
    buckets.push_back(Bucket{key.name, position, 0});
    // Type and attribute keys may be lowercased copies; count their text.
    keyBytes = key.kind == Kind::Attribute || key.kind == Kind::Type ? key.name.size() : 0;
  }
  ++buckets.back().count;
  return keyBytes;
}

const RuleIndex::Bucket* RuleIndex::findBucket(const std::vector<Bucket>& buckets,
                                               std::string_view key, bool lowercase) {
  const auto compare = [lowercase](std::string_view stored, std::string_view probe) {
    return lowercase ? CompareLowercased(stored, probe) : CompareExact(stored, probe);
  };
  const auto it = std::lower_bound(
      buckets.begin(), buckets.end(), key,
      [&](const Bucket& bucket, std::string_view probe) { return compare(bucket.key, probe) < 0; });
  return it == buckets.end() || compare(it->key, key) != 0 ? nullptr : &*it;
}

void RuleIndex::RemoveRepeatedHits(BucketHits& hits) {
  std::sort(hits.begin(), hits.end());
  hits.resize(static_cast<std::size_t>(std::unique(hits.begin(), hits.end()) - hits.begin()));
}

std::size_t RuleIndex::gatherHits(BucketHits& hits, std::vector<std::uint32_t>& candidates) const {
  const auto universal = entries_.begin() + universalFirst_;
  candidates.assign(universal, universal + universalCount_);
  if (hits.size() == 0) {
    return candidates.size();
  }
  // Each bucket once, so at most entryCount() positions are gathered whatever the element carries.
  RemoveRepeatedHits(hits);
  for (const auto& [firstEntry, count] : hits) {
    const auto first = entries_.begin() + firstEntry;
    candidates.insert(candidates.end(), first, first + count);
  }
  const std::size_t gathered = candidates.size();
  // A rule can sit in several of the element's buckets, as in `.a, rect`; it is tested once.
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
  return gathered;
}

}  // namespace donner::svg::components
