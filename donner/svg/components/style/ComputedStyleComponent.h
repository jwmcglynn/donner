#pragma once
/// @file

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "donner/base/Utils.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::svg::components {

class StyleResourceBudget;

/** Shared retained-byte account for immutable computed-style records in one document. */
class RetainedStyleBytes {
public:
  /**
   * Create an account with a document-local limit and optional document-family envelope.
   * @param limit Maximum retained computed-style bytes in this document.
   * @param family Optional shared envelope for this document family.
   */
  explicit RetainedStyleBytes(std::size_t limit,
                              std::shared_ptr<DocumentResourceFamilyBudget> family = nullptr)
      : limit_(limit), family_(std::move(family)) {}

  /** Return whether a reservation fits without changing counters or refusal state. */
  [[nodiscard]] bool canReserve(std::size_t bytes) const {
    const std::size_t current = retainedBytes_.load(std::memory_order_relaxed);
    return current <= limit_ && bytes <= limit_ - current &&
           (!family_ ||
            family_->canReserve(DocumentResourceFamilyBudget::Kind::ComputedStyle, bytes));
  }

  /** Reserve bytes for one newly created record. */
  [[nodiscard]] bool reserve(std::size_t bytes) {
    std::size_t current = retainedBytes_.load(std::memory_order_relaxed);
    do {
      if (current > limit_ || bytes > limit_ - current) {
        return false;
      }
    } while (
        !retainedBytes_.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed));
    if (family_ && !family_->reserve(DocumentResourceFamilyBudget::Kind::ComputedStyle, bytes)) {
      retainedBytes_.fetch_sub(bytes, std::memory_order_relaxed);
      return false;
    }
    return true;
  }

  /** Release bytes when the last reference to a charged record is destroyed. */
  void release(std::size_t bytes) {
    if (family_) {
      family_->release(DocumentResourceFamilyBudget::Kind::ComputedStyle, bytes);
    }
    std::size_t current = retainedBytes_.load(std::memory_order_relaxed);
    do {
      UTILS_RELEASE_ASSERT_MSG(current >= bytes, "Retained style byte underflow");
    } while (
        !retainedBytes_.compare_exchange_weak(current, current - bytes, std::memory_order_relaxed));
  }

  /// Current retained bytes.
  [[nodiscard]] std::size_t retainedBytes() const {
    return retainedBytes_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<std::size_t> retainedBytes_ = 0;
  std::size_t limit_;
  std::shared_ptr<DocumentResourceFamilyBudget> family_;
};

/// Final resource opacities for this style instance, including explicit parent inheritance.
struct ComputedResourceOpacities {
  double stopOpacity = 1;             ///< Final gradient stop opacity.
  double floodOpacity = 1;            ///< Final filter flood opacity.
  css::Specificity stopSpecificity;   ///< Winning stop-opacity cascade priority.
  css::Specificity floodSpecificity;  ///< Winning flood-opacity cascade priority.

  /// Equality operator.
  bool operator==(const ComputedResourceOpacities&) const = default;
};

/**
 * An element's computed style: the result of the `style=""` attribute, presentation attributes,
 * the CSS stylesheets and inheritance from the parent. A record is built once and never changes
 * after it is stored, so a \ref ComputedStyleComponent and any snapshot that holds it can share it.
 */
struct ComputedStyle {
  /// An empty style, for an element without local declarations.
  ComputedStyle() = default;

  /**
   * A style that starts from an element's local declarations.
   * @param local Presentation attributes and `style=""` declarations of the element.
   */
  explicit ComputedStyle(const PropertyRegistry& local) : properties(local) {}

  ~ComputedStyle() {
    if (retainedStyleBytes_ && chargedBytes_ != 0) {
      retainedStyleBytes_->release(chargedBytes_);
    }
  }

  /** Copy style values without copying the source record's lifetime charge. */
  ComputedStyle(const ComputedStyle& other)
      : properties(other.properties), resourceOpacities(other.resourceOpacities) {}
  ComputedStyle& operator=(const ComputedStyle&) = delete;
  ComputedStyle(ComputedStyle&&) = delete;
  ComputedStyle& operator=(ComputedStyle&&) = delete;

  /// The computed style properties.
  PropertyRegistry properties;
  /// Absent for manually constructed styles; the cascade always populates it.
  std::optional<ComputedResourceOpacities> resourceOpacities;

private:
  friend class StyleResourceBudget;

  /** Attach this record's only lifetime charge to its document account. */
  [[nodiscard]] bool retainBytes(const std::shared_ptr<RetainedStyleBytes>& account,
                                 std::size_t bytes) {
    if (retainedStyleBytes_) {
      return false;
    }
    if (bytes != 0 && !account->reserve(bytes)) {
      return false;
    }
    retainedStyleBytes_ = account;
    chargedBytes_ = bytes;
    return true;
  }

  std::shared_ptr<RetainedStyleBytes> retainedStyleBytes_;
  std::size_t chargedBytes_ = 0;
};

/**
 * Holds an element's computed style, which is a combination of the `style=""` attribute, the CSS
 * stylesheet, and the CSS cascade where properties are inherited from the parent. The style is a
 * shared immutable \ref ComputedStyle record, read through const accessors.
 */
struct ComputedStyleComponent {
  /// Small pages keep the storage of a document with few elements, such as an icon or each
  /// externally referenced document, to one 64-entry page, while paged storage keeps every
  /// component at a stable address as the storage grows.
  static constexpr std::size_t page_size = 64;

  /// Computed properties, or null while the style is not computed.
  [[nodiscard]] const PropertyRegistry* properties() const {
    return style ? &style->properties : nullptr;
  }

  /// Final resource opacities, or null while the style is not computed or for a manually
  /// constructed style without them.
  [[nodiscard]] const ComputedResourceOpacities* resourceOpacities() const {
    return style && style->resourceOpacities ? &*style->resourceOpacities : nullptr;
  }

  /// The computed style record, or null while the style is not computed. Records are shared and
  /// immutable: computing a style again stores a new record.
  std::shared_ptr<const ComputedStyle> style;
  bool externalFill = false;    ///< Ordinary fill inherited from the scoped external-use host.
  bool externalStroke = false;  ///< Ordinary stroke inherited from the scoped external-use host.
};

// A stored record may be shared by other entities and by render snapshots, so nothing reachable
// from a component may give mutable access to it.
static_assert(
    std::is_same_v<decltype(ComputedStyleComponent::style), std::shared_ptr<const ComputedStyle>>,
    "Computed style records are shared and must stay immutable");
static_assert(std::is_same_v<decltype(std::declval<ComputedStyleComponent&>().properties()),
                             const PropertyRegistry*>,
              "Computed properties are read through a const accessor");

}  // namespace donner::svg::components
