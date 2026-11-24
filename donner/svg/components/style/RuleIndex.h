#pragma once
/// @file

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/base/SmallVector.h"
#include "donner/css/Stylesheet.h"
#include "donner/css/selectors/ComplexSelector.h"

namespace donner::svg::components {

/**
 * The bucket one selector-list entry is filed under in a \ref RuleIndex: a simple selector of its
 * subject (rightmost) compound that every element it matches must carry.
 */
struct RuleIndexKey {
  /// What the key names, in order of preference when a compound has several.
  enum class Kind : std::uint8_t {
    Id,         //!< `#name`, compared exactly.
    Class,      //!< `.name`, compared exactly.
    Attribute,  //!< `[name]` with no namespace, compared as an ASCII-lowercased local name.
    Type,       //!< A type name, compared ASCII-lowercased; a universal type gives no key.
    Universal,  //!< No key: the entry is a candidate for every element.
  };

  Kind kind = Kind::Universal;  //!< What \ref name names.
  RcString name;                //!< Key text, lowercased for \ref Kind::Attribute and Kind::Type.

  /// Equality operator.
  bool operator==(const RuleIndexKey&) const = default;

  /// Print the key, for test failure output.
  friend std::ostream& operator<<(std::ostream& os, const RuleIndexKey& key);
};

/**
 * An element as \ref RuleIndex::collectCandidates reads it. Each accessor is called only when the
 * index has keys of that kind, so an element type can compute them on demand:
 *
 * - `std::string_view id() const`: the element's id, or empty.
 * - `forEachClassName(visit)`: calls `visit(std::string_view)` per `class` list entry.
 * - `forEachAttributeLocalName(visit)`: calls `visit(std::string_view)` per attribute.
 * - `std::string_view typeName() const`: the local tag name, in any case.
 *
 * The views only need to stay valid for the duration of the call.
 */
template <typename T>
concept RuleIndexElement = requires(const T& element, void (*visit)(std::string_view)) {
  { element.id() } -> std::convertible_to<std::string_view>;
  { element.typeName() } -> std::convertible_to<std::string_view>;
  element.forEachClassName(visit);
  element.forEachAttributeLocalName(visit);
};

/// Selector keys held by value, for tests and callers that already have them split.
struct RuleIndexElementKeys {
  std::string_view idName;                               //!< The element's id, or empty.
  SmallVector<std::string_view, 4> classNames;           //!< The `class` list, split on whitespace.
  std::string_view tag;                                  //!< Local tag name, in any case.
  SmallVector<std::string_view, 8> attributeLocalNames;  //!< Local names of its attributes.

  /// \ref RuleIndexElement accessors.
  std::string_view id() const { return idName; }
  /// \ref RuleIndexElement accessors.
  std::string_view typeName() const { return tag; }
  /// \ref RuleIndexElement accessors.
  template <typename Visitor>
  void forEachClassName(Visitor&& visit) const {
    for (std::string_view name : classNames) {
      visit(name);
    }
  }
  /// \ref RuleIndexElement accessors.
  template <typename Visitor>
  void forEachAttributeLocalName(Visitor&& visit) const {
    for (std::string_view name : attributeLocalNames) {
      visit(name);
    }
  }
};

/**
 * Candidate rules of one stylesheet, bucketed by a key of each selector-list entry's subject
 * compound. Every key is a necessary condition for its entry to match, so the candidates of an
 * element include every rule that can match it; the matcher still tests each candidate in full.
 *
 * Built once per parsed stylesheet. It stores rule positions, not pointers, so it stays valid
 * while the stylesheet it was built from is unchanged and must be rebuilt when it changes.
 */
class RuleIndex {
public:
  /// An empty index, for a stylesheet with no rules.
  RuleIndex() = default;

  /**
   * Build the index of \p rules.
   *
   * @param rules Rules of one stylesheet, in cascade order.
   */
  explicit RuleIndex(std::span<const css::SelectorRule> rules);

  /**
   * The key one selector-list entry is filed under: the subject compound's id, else a class,
   * else an attribute local name without a namespace, else a non-universal type name.
   * Pseudo-classes and pseudo-elements give no key, and neither do their arguments.
   *
   * @param selector One entry of a rule's selector list.
   */
  static RuleIndexKey KeyFor(const css::ComplexSelector& selector);

  /**
   * Replace \p candidates with the positions of the rules that can match \p element, ascending
   * and without duplicates.
   *
   * Each bucket is gathered at most once per element, however often the element names its key:
   * a repeated class token, or attribute names that differ only in case. Every filed entry sits in
   * exactly one bucket or the universal span, so at most \ref entryCount positions are gathered
   * before duplicates are removed, and \p candidates never grows past that. The remaining work
   * is one lookup per key the element carries, which is linear in the element's own source.
   *
   * @param element The element, read only for the key kinds this index has.
   * @param candidates Receives the rule positions; its capacity is reused.
   * @return Rule positions gathered before duplicates were removed, at most \ref entryCount.
   */
  template <RuleIndexElement Element>
  std::size_t collectCandidates(const Element& element,
                                std::vector<std::uint32_t>& candidates) const {
    BucketHits hits;
    if (!ids_.empty()) {
      if (const std::string_view id = element.id(); !id.empty()) {
        addHit(findBucket(ids_, id, false), hits);
      }
    }
    if (!classes_.empty()) {
      element.forEachClassName(
          [&](std::string_view name) { addHit(findBucket(classes_, name, false), hits); });
    }
    if (!attributes_.empty()) {
      element.forEachAttributeLocalName(
          [&](std::string_view name) { addHit(findBucket(attributes_, name, true), hits); });
    }
    if (!types_.empty()) {
      addHit(findBucket(types_, element.typeName(), true), hits);
    }
    return gatherHits(hits, candidates);
  }

  /// Number of rules the index was built from.
  std::size_t ruleCount() const { return ruleCount_; }

  /// Number of filed entries: one per selector-list entry, less entries of one rule with the same
  /// key.
  std::size_t entryCount() const { return entries_.size(); }

  /// Bytes the index retains: vector capacities, plus the text of type and attribute keys, which
  /// may be lowercased copies.
  std::size_t retainedBytes() const { return retainedBytes_; }

private:
  /// The rules filed under one key: `entries_[firstEntry, firstEntry + count)`.
  struct Bucket {
    RcString key;              //!< Key text; shares the selector's storage unless lowercased.
    std::uint32_t firstEntry;  //!< First position in \ref entries_.
    std::uint32_t count;       //!< Number of rule positions.
  };

  /// A bucket an element's key found: its first position in \ref entries_, which identifies the
  /// bucket across all key kinds, and its entry count.
  using BucketHit = std::pair<std::uint32_t, std::uint32_t>;
  /// Buckets found so far for one element; may repeat a bucket until compacted.
  using BucketHits = SmallVector<BucketHit, 16>;

  /// The bucket filed under \p key, or nullptr; \p lowercase compares ignoring ASCII case.
  static const Bucket* findBucket(const std::vector<Bucket>& buckets, std::string_view key,
                                  bool lowercase);
  /// Record \p bucket, if any, in \p hits. Repeats are removed whenever \p hits outgrows twice the
  /// bucket count, so it holds fewer than about three entries per bucket however many keys the
  /// element carries.
  void addHit(const Bucket* bucket, BucketHits& hits) const {
    if (bucket != nullptr) {
      hits.push_back(BucketHit{bucket->firstEntry, bucket->count});
      if (hits.size() > compactHitsAt_) {
        RemoveRepeatedHits(hits);
      }
    }
  }
  /// Sort \p hits and remove repeated buckets.
  static void RemoveRepeatedHits(BucketHits& hits);
  /// Replace \p candidates with the universal positions and those of each distinct bucket in
  /// \p hits, ascending and without duplicates; returns the positions gathered before duplicates
  /// were removed.
  std::size_t gatherHits(BucketHits& hits, std::vector<std::uint32_t>& candidates) const;
  /// The bucket list for keys of \p kind, which is not \ref RuleIndexKey::Kind::Universal.
  std::vector<Bucket>& bucketsFor(RuleIndexKey::Kind kind);
  /// File one entry, in sorted order; returns the key text bytes it added.
  std::size_t addEntry(const RuleIndexKey& key, std::uint32_t rule);

  std::vector<Bucket> ids_;             //!< Sorted by key.
  std::vector<Bucket> classes_;         //!< Sorted by key.
  std::vector<Bucket> attributes_;      //!< Sorted by lowercased key.
  std::vector<Bucket> types_;           //!< Sorted by lowercased key.
  std::vector<std::uint32_t> entries_;  //!< Rule positions, ascending within each bucket.
  std::uint32_t universalFirst_ = 0;    //!< First universal position in \ref entries_.
  std::uint32_t universalCount_ = 0;    //!< Number of universal positions.
  std::size_t ruleCount_ = 0;           //!< Rules the index was built from.
  std::size_t retainedBytes_ = 0;       //!< See \ref retainedBytes.
  std::size_t compactHitsAt_ = 0;       //!< Hit count past which \ref addHit removes repeats.
};

}  // namespace donner::svg::components
