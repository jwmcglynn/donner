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
  // The new chain, innermost first. Its length is bounded by the document's nesting depth.
  target_.clear();
  for (Entity ancestor = StyleParentElement(registry_, treeEntity); ancestor != entt::null;
       ancestor = StyleParentElement(registry_, ancestor)) {
    target_.push_back(ancestor);
  }
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
  appendKeyHashes(treeEntity);
  chain_.push_back(
      Frame{treeEntity, firstHash, static_cast<std::uint32_t>(hashes_.size()) - firstHash});
  if (disabledAt_ == kEnabled && !insertFrame(chain_.back())) {
    disabledAt_ = chain_.size() - 1;
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
      [[maybe_unused]] const bool inserted = insertFrame(remaining);
      assert(inserted && "a prefix of an enabled chain inserts without disabling");
    }
  }
}

bool StyleAncestorFilter::insertFrame(const Frame& frame) {
  if (frame.hashCount > kMaxKeysPerAncestor) {
    filter_.clear();
    return false;
  }
  for (std::uint32_t i = 0; i < frame.hashCount; ++i) {
    if (filter_.insert(hashes_[frame.firstHash + i])) {
      // A saturated counter could never be decremented again; clear on the first report.
      filter_.clear();
      return false;
    }
  }
  return true;
}

void StyleAncestorFilter::appendKeyHashes(Entity treeEntity) {
  const RuleIndexElementView element(registry_, treeEntity, StyleDataEntity(registry_, treeEntity));
  // One key past the cap is enough to disable the filter, so later keys are neither lowercased
  // nor hashed.
  const std::size_t end = hashes_.size() + kMaxKeysPerAncestor + 1;
  const auto room = [&] { return hashes_.size() < end; };
  hashes_.push_back(lowercasedHash(Kind::Type, element.typeName()));
  if (const std::string_view id = element.id(); !id.empty()) {
    hashes_.push_back(RuleIndex::AncestorKeyHash(Kind::Id, id));
  }
  element.forEachClassName([&](std::string_view name) {
    if (room()) {
      hashes_.push_back(RuleIndex::AncestorKeyHash(Kind::Class, name));
    }
  });
  element.forEachAttributeLocalName([&](std::string_view name) {
    if (room()) {
      hashes_.push_back(lowercasedHash(Kind::Attribute, name));
    }
  });
}

BloomHash StyleAncestorFilter::lowercasedHash(RuleIndexKey::Kind kind, std::string_view name) {
  lowercase_.assign(name);
  for (char& ch : lowercase_) {
    ch = StringUtils::ToLowerAscii(ch);
  }
  return RuleIndex::AncestorKeyHash(kind, lowercase_);
}

}  // namespace donner::svg::components
