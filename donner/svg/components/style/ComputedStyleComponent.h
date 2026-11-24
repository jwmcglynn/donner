#pragma once
/// @file

#include <cstddef>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::svg::components {

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

  /// The computed style properties.
  PropertyRegistry properties;
  /// Absent for manually constructed styles; the cascade always populates it.
  std::optional<ComputedResourceOpacities> resourceOpacities;
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
