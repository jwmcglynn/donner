#include "donner/svg/components/style/RuleIndex.h"

#include <algorithm>
#include <span>
#include <tuple>
#include <utility>
#include <variant>

#include "donner/base/StringUtils.h"

namespace donner::svg::components {
namespace {

using Kind = RuleIndexKey::Kind;

bool HasAsciiUppercase(std::string_view text) {
  return std::any_of(text.begin(), text.end(), [](char ch) { return ch >= 'A' && ch <= 'Z'; });
}

/// Compare \p stored (already lowercased) with \p probe lowercased, as `<=>` would.
int CompareLowercased(std::string_view stored, std::string_view probe) {
  const std::size_t length = std::min(stored.size(), probe.size());
  for (std::size_t i = 0; i < length; ++i) {
    const char lhs = stored[i];
    const char rhs = StringUtils::ToLowerAscii(probe[i]);
    if (lhs != rhs) {
      return static_cast<unsigned char>(lhs) < static_cast<unsigned char>(rhs) ? -1 : 1;
    }
  }
  return stored.size() == probe.size() ? 0 : (stored.size() < probe.size() ? -1 : 1);
}

int CompareExact(std::string_view stored, std::string_view probe) {
  const int result = stored.compare(probe);
  return result < 0 ? -1 : (result > 0 ? 1 : 0);
}

/// One selector-list entry filed under a key, before buckets are formed.
struct FiledEntry {
  RuleIndexKey key;
  std::uint32_t rule;
};

/// The first simple selector of type \p Selector in \p simple that \p accept admits.
template <typename Selector, typename Accept>
const Selector* FindSimple(const std::vector<css::CompoundSelector::Entry>& simple, Accept accept) {
  for (const auto& entry : simple) {
    const auto* selector = std::get_if<Selector>(&entry);
    if (selector != nullptr && accept(*selector)) {
      return selector;
    }
  }
  return nullptr;
}

/// Admits every simple selector of the requested type.
constexpr auto kAnySelector = [](const auto& /*selector*/) { return true; };

bool HasNoNamespace(const css::AttributeSelector& selector) {
  return selector.name.name.namespacePrefix.empty();
}

bool IsNamedType(const css::TypeSelector& selector) {
  return !selector.isUniversal();
}

RcString Lowercased(const RcString& name) {
  return HasAsciiUppercase(name) ? RcString(StringUtils::ToLowerAscii(std::string_view(name)))
                                 : name;
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

}  // namespace

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

RuleIndex::RuleIndex(std::span<const css::SelectorRule> rules) : ruleCount_(rules.size()) {
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
  retainedBytes_ =
      entries_.capacity() * sizeof(std::uint32_t) +
      (ids_.capacity() + classes_.capacity() + attributes_.capacity() + types_.capacity()) *
          sizeof(Bucket) +
      keyBytes;
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
