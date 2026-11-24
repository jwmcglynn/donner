#include "donner/svg/components/style/StyleAncestorFilter.h"

#include <algorithm>
#include <cassert>

#include "donner/base/StringUtils.h"
#include "donner/svg/components/style/RuleIndex.h"
#include "donner/svg/components/style/StyleElementView.h"

namespace donner::svg::components {

using Kind = RuleIndexKey::Kind;

StyleAncestorFilter::StyleAncestorFilter(const Registry& registry) : registry_(registry) {}

void StyleAncestorFilter::synchronizeTo(Entity treeEntity) {
  // Keep the last complete chain intact when a DOM or shadow chain exceeds the limit.
  target_.clear();
  for (Entity ancestor = StyleParentElement(registry_, treeEntity); ancestor != entt::null;
       ancestor = StyleParentElement(registry_, ancestor)) {
    if (target_.size() == kMaxAncestorDepth) {
      depthLimitExceeded_ = true;
      return;
    }
    target_.push_back(ancestor);
  }
  depthLimitExceeded_ = false;
  std::reverse(target_.begin(), target_.end());

  std::size_t shared = 0;
  while (shared < chain_.size() && shared < target_.size() &&
         chain_[shared].treeEntity == target_[shared]) {
    ++shared;
  }
  while (chain_.size() > shared) {
    pop();
  }
  for (std::size_t i = shared; i < target_.size(); ++i) {
    push(target_[i]);
  }
}

void StyleAncestorFilter::push(Entity treeEntity) {
  const auto firstHash = static_cast<std::uint32_t>(hashes_.size());
  // Descendants of the disabling ancestor leave the chain before it can be rebuilt.
  const bool keysComplete = disabledAt_ == kEnabled && appendKeyHashes(treeEntity);
  chain_.push_back(Frame{treeEntity, firstHash,
                         static_cast<std::uint32_t>(hashes_.size()) - firstHash, keysComplete});
  if (disabledAt_ != kEnabled) {
    return;
  }
  const InsertResult result = insertFrame(chain_.back());
  if (result != InsertResult::Inserted) {
    disabledAt_ = chain_.size() - 1;
    ++(result == InsertResult::Saturated ? saturationDisables_ : keyLimitDisables_);
  }
}

void StyleAncestorFilter::pop() {
  const Frame frame = chain_.back();
  chain_.pop_back();
  if (disabledAt_ == kEnabled) {
    for (std::uint32_t i = 0; i < frame.hashCount; ++i) {
      filter_.remove(hashes_[frame.firstHash + i]);
    }
  }
  hashes_.erase(hashes_.begin() + frame.firstHash, hashes_.end());
  if (disabledAt_ == chain_.size()) {
    // The disabling ancestor left the chain: rebuild from the rest, which inserted without
    // disabling before, so it does so again.
    disabledAt_ = kEnabled;
    filter_.clear();
    for (const Frame& remaining : chain_) {
      [[maybe_unused]] const InsertResult result = insertFrame(remaining);
      assert(result == InsertResult::Inserted && "a prefix of an enabled chain inserts");
    }
  }
}

StyleAncestorFilter::InsertResult StyleAncestorFilter::insertFrame(const Frame& frame) {
  if (!frame.keysComplete) {
    filter_.clear();
    return InsertResult::KeyLimit;
  }
  for (std::uint32_t i = 0; i < frame.hashCount; ++i) {
    if (filter_.insert(hashes_[frame.firstHash + i])) {
      // A saturated counter could never be decremented again; clear on the first report.
      filter_.clear();
      return InsertResult::Saturated;
    }
  }
  return InsertResult::Inserted;
}

bool StyleAncestorFilter::appendKeyHashes(Entity treeEntity) {
  const RuleIndexElementView element(registry_, treeEntity, StyleDataEntity(registry_, treeEntity));
  const std::string_view id = element.id();
  const std::string_view classes = element.className();
  const std::size_t attributes = element.attributeCount();
  const std::size_t fixedKeys = 1 + !id.empty();
  if (attributes > kMaxKeysPerAncestor - fixedKeys || classes.size() > kMaxKeyBytesPerAncestor) {
    return false;
  }

  std::size_t remainingBytes = kMaxKeyBytesPerAncestor - classes.size();
  if (!appendKeyHash(Kind::Type, element.typeName(), remainingBytes) ||
      (!id.empty() && !appendKeyHash(Kind::Id, id, remainingBytes))) {
    return false;
  }

  std::size_t remainingClasses = kMaxKeysPerAncestor - fixedKeys - attributes;
  for (const std::string_view name : StringUtils::SplitAsciiWhitespace(classes)) {
    if (remainingClasses == 0) {
      return false;
    }
    --remainingClasses;
    // The raw class text was already reserved above.
    hashes_.push_back(RuleIndex::AncestorKeyHash(Kind::Class, name));
  }

  bool complete = true;
  element.forEachAttributeLocalName([&](std::string_view name) {
    complete = complete && appendKeyHash(Kind::Attribute, name, remainingBytes);
  });
  return complete;
}

bool StyleAncestorFilter::appendKeyHash(Kind kind, std::string_view name,
                                        std::size_t& remainingBytes) {
  if (name.size() > remainingBytes) {
    return false;
  }
  remainingBytes -= name.size();
  hashes_.push_back(kind == Kind::Id ? RuleIndex::AncestorKeyHash(kind, name)
                                     : lowercasedHash(kind, name));
  return true;
}

BloomHash StyleAncestorFilter::lowercasedHash(RuleIndexKey::Kind kind, std::string_view name) {
  lowercase_.assign(name);
  for (char& ch : lowercase_) {
    ch = StringUtils::ToLowerAscii(ch);
  }
  return RuleIndex::AncestorKeyHash(kind, lowercase_);
}

}  // namespace donner::svg::components
