#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/BloomFilter.h"
#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/style/RuleIndex.h"

namespace donner::svg::components {

/**
 * The keys of an element's ancestors, as a counting Bloom filter: for each ancestor on the chain
 * selector matching walks (\ref StyleParentElement), its lowercased tag name, its id, each class
 * and each lowercased attribute local name, hashed with \ref RuleIndex::AncestorKeyHash. A rule
 * whose every selector-list entry needs an ancestor key the filter does not contain cannot match,
 * so its ancestor walk can be skipped.
 *
 * \ref synchronizeTo makes the filter hold exactly an element's ancestors, whatever element it
 * held before: it pops the ancestors not on the new chain, removing their keys, and pushes the
 * missing ones from the outermost down. Its contents, and whether it is enabled, are therefore a
 * function of the element's ancestor chain alone, so the filter is correct whatever order the
 * elements of a pass are visited in, and a pass may start anywhere in the tree.
 *
 * The filter is disabled (every probe answers "may contain") below an ancestor whose insertion
 * saturated a counter, or which has more than \ref kMaxKeysPerAncestor keys, until synchronization
 * pops that ancestor; it is then rebuilt from the remaining chain. A saturated counter could never
 * be decremented, which would make the contents depend on history; the key limit bounds the work
 * one ancestor costs.
 *
 * One filter serves one style pass on one thread; it reads the registry but never writes it.
 */
class StyleAncestorFilter {
public:
  /// Most keys one ancestor may contribute before the filter is disabled below it.
  static constexpr std::size_t kMaxKeysPerAncestor = 64;

  /// Longer ancestor chains use ordinary selector matching.
  static constexpr std::size_t kMaxAncestorDepth = 256;

  /// Combined byte limit for class text and the other key names of one ancestor.
  static constexpr std::size_t kMaxKeyBytesPerAncestor = 4096;

  /**
   * An empty, enabled filter for elements of \p registry.
   *
   * @param registry Registry whose elements are synchronized to; must outlive the filter.
   */
  explicit StyleAncestorFilter(const Registry& registry);

  /**
   * Make the filter hold the ancestors of \p treeEntity, as selector matching walks them.
   *
   * @param treeEntity The element about to be matched: a tree entity, document or shadow.
   */
  void synchronizeTo(Entity treeEntity);

  /**
   * False only if no ancestor of the element synchronized to has the key of \p hash. Always true
   * while the filter is disabled.
   *
   * @param hash A \ref RuleIndex::AncestorKeyHash value.
   */
  bool mayContain(BloomHash hash) const { return !enabled() || filter_.mayContain(hash); }

  /// Whether the filter can reject anything for the element synchronized to.
  bool enabled() const { return !depthLimitExceeded_ && disabledAt_ == kEnabled; }

  /// Number of ancestors held: the depth of the element synchronized to.
  std::size_t depth() const { return depthLimitExceeded_ ? 0 : chain_.size(); }

  /// Times pushing an ancestor disabled the filter because an insertion saturated a counter.
  std::uint64_t saturationDisables() const { return saturationDisables_; }

  /// Times pushing an ancestor disabled the filter because it had more than
  /// \ref kMaxKeysPerAncestor keys or too many key bytes.
  std::uint64_t keyLimitDisables() const { return keyLimitDisables_; }

private:
  /// One ancestor and the hashes of its keys, `hashes_[firstHash, firstHash + hashCount)`.
  struct Frame {
    Entity treeEntity;
    std::uint32_t firstHash;
    std::uint32_t hashCount;
    bool keysComplete;
  };

  /// \ref disabledAt_ value while the filter is enabled.
  static constexpr std::size_t kEnabled = static_cast<std::size_t>(-1);

  /// Append the ancestor \p treeEntity, inserting its keys unless the filter is disabled.
  void push(Entity treeEntity);
  /// Remove the innermost ancestor, re-enabling and rebuilding the filter if it disabled it.
  void pop();
  /// Append the ancestor's keys, returning false before exceeding a count or byte limit.
  bool appendKeyHashes(Entity treeEntity);
  /// Append a non-class key if its name fits the remaining ancestor budget.
  bool appendKeyHash(RuleIndexKey::Kind kind, std::string_view name, std::size_t& remainingBytes);
  /// The ancestor key hash of \p name ASCII-lowercased, lowercased in \ref lowercase_.
  BloomHash lowercasedHash(RuleIndexKey::Kind kind, std::string_view name);
  /// Why inserting a frame disabled the filter, if it did.
  enum class InsertResult : std::uint8_t { Inserted, Saturated, KeyLimit };
  /// Insert the hashes of \p frame; on any other result than `Inserted` the filter is cleared.
  InsertResult insertFrame(const Frame& frame);

  const Registry& registry_;              //!< Registry of the synchronized elements.
  CountingBloomFilter<12> filter_;        //!< Keys of the enabled prefix of \ref chain_.
  std::vector<Frame> chain_;              //!< Ancestors, outermost first.
  std::vector<BloomHash> hashes_;         //!< Key hashes of \ref chain_, frame by frame.
  std::size_t disabledAt_ = kEnabled;     //!< Index in \ref chain_ of the disabling ancestor.
  bool depthLimitExceeded_ = false;       //!< The requested chain exceeds the depth limit.
  std::vector<Entity> target_;            //!< Reused: the chain being synchronized to.
  std::string lowercase_;                 //!< Reused: a name being lowercased.
  std::uint64_t saturationDisables_ = 0;  //!< See \ref saturationDisables.
  std::uint64_t keyLimitDisables_ = 0;    //!< See \ref keyLimitDisables.
};

}  // namespace donner::svg::components
