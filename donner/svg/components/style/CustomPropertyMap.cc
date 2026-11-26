#include "donner/svg/components/style/CustomPropertyMap.h"

#include <algorithm>
#include <concepts>
#include <type_traits>

#include "donner/svg/components/style/ComputedStyleComponent.h"

namespace donner::svg::components {
namespace {

class AllocationCharge {
public:
  explicit AllocationCharge(std::shared_ptr<RetainedStyleBytes> account)
      : account_(std::move(account)) {}

  ~AllocationCharge() {
    if (bytes_) {
      account_->release(bytes_);
    }
  }

  AllocationCharge(const AllocationCharge&) = delete;
  AllocationCharge& operator=(const AllocationCharge&) = delete;

  bool cover(std::size_t bytes) {
    if (bytes <= bytes_) {
      return true;
    }
    if (!account_ || !account_->reserve(bytes - bytes_)) {
      return false;
    }
    bytes_ = bytes;
    return true;
  }

  template <typename T>
  std::shared_ptr<const T> own(T* value) {
    const auto bytes = std::exchange(bytes_, 0);
    return std::shared_ptr<const T>(value, [account = std::move(account_), bytes](const T* object) {
      delete object;
      account->release(bytes);
    });
  }

private:
  std::shared_ptr<RetainedStyleBytes> account_;
  std::size_t bytes_ = 0;
};

struct StorageCost {
  std::size_t bytes;
  std::size_t text = 0;
  std::size_t values = 0;
  bool valid = true;

  void addString(const RcString& string) {
    if (string.size() > TokenSequence::kMaximumTextBytes - text) {
      valid = false;
      return;
    }
    text += string.size();
    // Conservatively cover the owned character array, terminator and backing vector.
    bytes += string.size() + 1 + sizeof(std::vector<char>);
  }
};

template <typename T, typename Visit>
void VisitTokenStrings(T& token, const Visit& visit) {
  if constexpr (requires {
                  { std::as_const(token.value) } -> std::same_as<const RcString&>;
                }) {
    visit(token.value);
  }
  if constexpr (requires {
                  { std::as_const(token.name) } -> std::same_as<const RcString&>;
                }) {
    visit(token.name);
  }
  if constexpr (requires { token.valueString; }) {
    visit(token.valueString);
  }
  if constexpr (requires { token.suffixString; }) {
    visit(token.suffixString);
  }
}

void MeasureTokens(std::span<const css::ComponentValue> values, std::size_t capacity,
                   StorageCost& cost, std::size_t depth, bool actualCapacity);

void MeasureNestedTokens(const css::ComponentValue& value, StorageCost& cost, std::size_t depth,
                         bool actualCapacity) {
  if (const auto* function = std::get_if<css::Function>(&value.value)) {
    cost.addString(function->name);
    MeasureTokens(function->values,
                  actualCapacity ? function->values.capacity() : function->values.size(), cost,
                  depth, actualCapacity);
  } else if (const auto* block = std::get_if<css::SimpleBlock>(&value.value)) {
    MeasureTokens(block->values, actualCapacity ? block->values.capacity() : block->values.size(),
                  cost, depth, actualCapacity);
  } else {
    std::get<css::Token>(value.value).visit([&](const auto& token) {
      VisitTokenStrings(token, [&](const RcString& string) { cost.addString(string); });
    });
  }
}

void MeasureTokens(std::span<const css::ComponentValue> values, std::size_t capacity,
                   StorageCost& cost, std::size_t depth, bool actualCapacity) {
  constexpr std::size_t kMaximumDepth = 64;
  if (!cost.valid || depth > kMaximumDepth ||
      values.size() > TokenSequence::kMaximumComponentValues - cost.values) {
    cost.valid = false;
    return;
  }
  cost.values += values.size();
  cost.bytes += capacity * sizeof(css::ComponentValue);
  for (const auto& value : values) {
    MeasureNestedTokens(value, cost, depth + 1, actualCapacity);
    if (!cost.valid) {
      break;
    }
  }
}

std::vector<css::ComponentValue> CloneTokens(std::span<const css::ComponentValue> values);

css::ComponentValue CloneToken(const css::ComponentValue& value) {
  return std::visit(
      [](const auto& part) -> css::ComponentValue {
        using T = std::decay_t<decltype(part)>;
        if constexpr (std::is_same_v<T, css::Function>) {
          T copy(RcString(std::string_view(part.name)), part.sourceOffset);
          copy.values = CloneTokens(part.values);
          return css::ComponentValue(std::move(copy));
        } else if constexpr (std::is_same_v<T, css::SimpleBlock>) {
          T copy(part.associatedToken, part.sourceOffset);
          copy.values = CloneTokens(part.values);
          return css::ComponentValue(std::move(copy));
        } else {
          auto copy = part;
          part.visit([&](const auto& token) {
            VisitTokenStrings(copy.template get<std::decay_t<decltype(token)>>(),
                              [](RcString& string) { string.dedup(); });
          });
          return css::ComponentValue(std::move(copy));
        }
      },
      value.value);
}

std::vector<css::ComponentValue> CloneTokens(std::span<const css::ComponentValue> values) {
  std::vector<css::ComponentValue> result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.push_back(CloneToken(value));
  }
  return result;
}

using Entry = CustomPropertyMap::Entry;
using Change = CustomPropertyMap::Change;

struct BorrowedEntry {
  const RcString* name;
  const std::shared_ptr<const TokenSequence>* value;
};

std::vector<const Change*> SortedChanges(std::span<const Change> changes) {
  std::vector<const Change*> sorted;
  sorted.reserve(changes.size());
  for (const auto& change : changes) {
    const std::string_view name(change.name);
    if (!name.starts_with("--") || name.size() == 2) {
      return {};
    }
    sorted.push_back(&change);
  }
  std::ranges::sort(sorted, [](const Change* a, const Change* b) {
    const int order = std::string_view(a->name).compare(std::string_view(b->name));
    return order == 0 ? a < b : order < 0;
  });
  return sorted;
}

void OverlayOne(std::vector<BorrowedEntry>& entries, const Change& change, const Entry* parent) {
  switch (change.action) {
    case CustomPropertyMap::Action::Set:
      if (change.value) {
        entries.push_back({&change.name, &change.value});
      }
      break;
    case CustomPropertyMap::Action::Initial: break;
    case CustomPropertyMap::Action::Inherit:
    case CustomPropertyMap::Action::Unset:
      if (parent) {
        entries.push_back({&parent->name, &parent->value});
      }
      break;
  }
}

std::vector<BorrowedEntry> MergeEntries(std::span<const Entry> inherited,
                                        std::span<const Change* const> sorted) {
  std::vector<BorrowedEntry> result;
  auto nextParent = inherited.begin();
  for (std::size_t i = 0; i < sorted.size(); ++i) {
    const auto& change = *sorted[i];
    if (i + 1 < sorted.size() && change.name == sorted[i + 1]->name) {
      continue;
    }
    while (nextParent != inherited.end() &&
           std::string_view(nextParent->name) < std::string_view(change.name)) {
      result.push_back({&nextParent->name, &nextParent->value});
      ++nextParent;
    }
    const Entry* parent = nullptr;
    if (nextParent != inherited.end() && nextParent->name == change.name) {
      parent = &*nextParent++;
    }
    OverlayOne(result, change, parent);
  }
  for (; nextParent != inherited.end(); ++nextParent) {
    result.push_back({&nextParent->name, &nextParent->value});
  }
  return result;
}

bool SameEntries(std::span<const BorrowedEntry> entries, std::span<const Entry> parent) {
  return std::ranges::equal(entries, parent, [](const BorrowedEntry& entry, const Entry& old) {
    return *entry.name == old.name && *entry.value == old.value;
  });
}

std::optional<std::vector<Entry>> CopyEntries(std::span<const BorrowedEntry> entries,
                                              AllocationCharge& charge) {
  StorageCost cost{sizeof(CustomPropertyMap)};
  for (const auto& entry : entries) {
    cost.addString(*entry.name);
  }
  if (!cost.valid || !charge.cover(cost.bytes + entries.size() * sizeof(Entry))) {
    return std::nullopt;
  }
  std::vector<Entry> owned;
  owned.reserve(entries.size());
  if (!charge.cover(cost.bytes + owned.capacity() * sizeof(Entry))) {
    return std::nullopt;
  }
  for (const auto& entry : entries) {
    owned.push_back({RcString(std::string_view(*entry.name)), *entry.value});
  }
  return owned;
}

}  // namespace

std::shared_ptr<const TokenSequence> TokenSequence::Create(
    std::shared_ptr<RetainedStyleBytes> account, std::span<const css::ComponentValue> values) {
  StorageCost projected{sizeof(TokenSequence)};
  MeasureTokens(values, values.size(), projected, 0, false);
  AllocationCharge charge(std::move(account));
  if (!projected.valid || !charge.cover(projected.bytes)) {
    return nullptr;
  }
  auto owned = CloneTokens(values);
  StorageCost actual{sizeof(TokenSequence)};
  MeasureTokens(owned, owned.capacity(), actual, 0, true);
  if (!actual.valid || !charge.cover(actual.bytes)) {
    return nullptr;
  }
  return charge.own(new TokenSequence(std::move(owned)));
}

std::optional<std::shared_ptr<const CustomPropertyMap>> CustomPropertyMap::Overlay(
    std::shared_ptr<RetainedStyleBytes> account, std::shared_ptr<const CustomPropertyMap> parent,
    std::span<const Change> changes) {
  if (changes.empty()) {
    return parent;
  }
  if (changes.size() > kMaximumEntries) {
    return std::nullopt;
  }
  const auto sorted = SortedChanges(changes);
  if (sorted.empty()) {
    return std::nullopt;
  }

  const auto inherited = parent ? parent->entries() : std::span<const Entry>();
  const auto merged = MergeEntries(inherited, sorted);
  if (SameEntries(merged, inherited)) {
    return parent;
  }
  if (merged.empty()) {
    return std::shared_ptr<const CustomPropertyMap>();
  }
  if (merged.size() > kMaximumEntries) {
    return std::nullopt;
  }

  AllocationCharge charge(std::move(account));
  auto owned = CopyEntries(merged, charge);
  if (!owned) {
    return std::nullopt;
  }
  return charge.own(new CustomPropertyMap(std::move(*owned)));
}

const TokenSequence* CustomPropertyMap::find(std::string_view name) const {
  const auto it = std::ranges::lower_bound(
      entries_, name, {}, [](const Entry& entry) { return std::string_view(entry.name); });
  return it != entries_.end() && name == std::string_view(it->name) ? it->value.get() : nullptr;
}

}  // namespace donner::svg::components
