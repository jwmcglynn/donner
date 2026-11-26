#pragma once
/// @file
/// Shared immutable storage for computed, untyped CSS custom property values.

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/base/Utils.h"
#include "donner/css/ComponentValue.h"

namespace donner::svg::components {

class RetainedStyleBytes;

/** Independently owned tokens whose style-byte reservation follows the last reference. */
class TokenSequence {
public:
  /// Maximum number of values in the entire token tree.
  static constexpr std::size_t kMaximumComponentValues = 65'536;
  /// Maximum aggregate stored token text.
  static constexpr std::size_t kMaximumTextBytes = 1024 * 1024;

  /**
   * Deep-copy an admitted token tree, detaching token strings from the declaration source.
   * @param account Account charged until the last reference releases the sequence.
   * @param values Borrowed values. An empty tree is a valid custom property value.
   * @return Null when a storage or retained-byte limit refuses the sequence.
   */
  [[nodiscard]] static std::shared_ptr<const TokenSequence> Create(
      std::shared_ptr<RetainedStyleBytes> account, std::span<const css::ComponentValue> values);

  /// Borrow the stored tokens for the lifetime of this sequence.
  [[nodiscard]] std::span<const css::ComponentValue> tokens() const UTILS_LIFETIME_BOUND {
    return tokens_;
  }

  TokenSequence(const TokenSequence&) = delete;
  TokenSequence& operator=(const TokenSequence&) = delete;

private:
  explicit TokenSequence(std::vector<css::ComponentValue> tokens) : tokens_(std::move(tokens)) {}

  std::vector<css::ComponentValue> tokens_;
};

/** Immutable case-sensitive names in sorted order, sharing independently charged token trees. */
class CustomPropertyMap {
public:
  /// Replacement applied to an inherited custom property.
  enum class Action {
    Set,      //!< Replace with the supplied tokens, or remove the name when the value is invalid.
    Initial,  //!< The guaranteed-invalid value, even if a parent has the name.
    Inherit,  //!< Keep the parent's value.
    Unset,    //!< Untyped custom properties inherit.
  };

  /// One winning local declaration. The last change wins when a name appears more than once.
  struct Change {
    RcString name;
    Action action;
    std::shared_ptr<const TokenSequence> value;
  };

  /// A present computed value.
  struct Entry {
    RcString name;
    std::shared_ptr<const TokenSequence> value;
  };

  /// Maximum number of names in one map or overlay input.
  static constexpr std::size_t kMaximumEntries = 65'536;

  /**
   * Overlay local declarations without modifying the inherited map or its tokens.
   * @param account Account for the new map and its names.
   * @param parent Inherited values, potentially owned by another document.
   * @param changes Local declarations in order.
   * @return Nullopt on refusal. An engaged null pointer represents an empty map.
   */
  [[nodiscard]] static std::optional<std::shared_ptr<const CustomPropertyMap>> Overlay(
      std::shared_ptr<RetainedStyleBytes> account, std::shared_ptr<const CustomPropertyMap> parent,
      std::span<const Change> changes);

  /**
   * Find one value by its exact name.
   * @param name Case-sensitive custom property name.
   * @return A borrowed sequence, or null for an absent or guaranteed-invalid value.
   */
  [[nodiscard]] const TokenSequence* find(std::string_view name) const UTILS_LIFETIME_BOUND;

  /// Borrow the sorted entries for the lifetime of this map.
  [[nodiscard]] std::span<const Entry> entries() const UTILS_LIFETIME_BOUND { return entries_; }

  CustomPropertyMap(const CustomPropertyMap&) = delete;
  CustomPropertyMap& operator=(const CustomPropertyMap&) = delete;

private:
  explicit CustomPropertyMap(std::vector<Entry> entries) : entries_(std::move(entries)) {}

  std::vector<Entry> entries_;
};

}  // namespace donner::svg::components
