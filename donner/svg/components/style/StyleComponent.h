#pragma once

#include <cstddef>
#include <functional>
#include <optional>
/// @file

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::svg::components {

/**
 * Contains properties the `style=""` attribute for an element, local to the element. This is used
 * during the CSS cascade, which stores the final element style in \ref ComputedStyleComponent.
 */
struct StyleComponent {
  /// Limit initial storage for this large component while retaining paged pointer stability.
  static constexpr std::size_t page_size = 64;

  /// Parsed local declarations, including presentation attributes.
  const PropertyRegistry& properties() const { return properties_; }

  /// Hash of the exact local declaration key; oversize local keys bypass the cache.
  std::optional<std::size_t> declarationHash() const { return declarationHash_; }

  /// Whether the local declaration key is identical to an absent style component.
  bool hasEmptyDeclarations() const { return empty_; }

  /// Change typed declarations and update the lookup hash after the mutation.
  template <typename F>
  void editProperties(F&& edit) {
    edit(properties_);
    refreshDeclarationHash();
  }

  /// Remove one presentation declaration and update the lookup hash when it changes.
  PropertyRegistry::PresentationAttributeClear clearPresentationAttribute(std::string_view name) {
    const auto result = properties_.clearPresentationAttribute(name);
    if (result == PropertyRegistry::PresentationAttributeClear::Cleared) {
      refreshDeclarationHash();
    }
    return result;
  }
  /// True when the most recent inline-style parse exceeded its resource budget.
  bool styleParseRejected = false;

  /**
   * Sets the properties from the value of the element's `style=""` attribute, replacing the
   * prior `style=""` contribution on this element. Presentation-attribute-origin properties
   * (e.g. `fill="red"`) and author-stylesheet properties are left intact - the replacement
   * is scoped to declarations previously tagged with
   * \ref css::Specificity::StyleAttribute().
   *
   * Safe to call from the SVG parse pipeline (where presentation attributes may already
   * be in the registry) and from the editor's text-edit rewrite path.
   *
   * Callers that want to *merge* new declarations into an existing `style=""` - e.g. a
   * tool that toggles one property - should use \ref updateStyle instead.
   *
   * @param style The value of the `style` attribute.
   */
  bool setStyle(std::string_view style) {
    PropertyRegistry candidate = properties_;
    candidate.clearStyleAttributeProperties();
    if (!candidate.parseStyle(style)) {
      styleParseRejected = true;
      return false;
    }
    properties_ = std::move(candidate);
    refreshDeclarationHash();
    styleParseRejected = false;
    return true;
  }

  /**
   * Updates the properties from the value of the element's `style=""` attribute, adding new
   * properties or updating existing ones. Does not remove existing `style=""` attribute
   * information.
   *
   * @param style The update style to apply, as a CSS style string (e.g. "fill:red;").
   */
  bool updateStyle(std::string_view style) {
    const bool accepted = properties_.parseStyle(style);
    refreshDeclarationHash();
    styleParseRejected = !accepted;
    return accepted;
  }

  /**
   * Tries to set a common presentation attribute (fill, stroke, opacity, transform, etc.).
   *
   * For element-specific attributes (cx, cy, r, d, etc.), callers should also invoke
   * \c parser::ParsePresentationAttribute separately.
   *
   * @param handle The entity to set the attribute on.
   * @param name The name of the attribute.
   * @param value The value of the attribute.
   * @return \c true if the attribute was set, \c false if the attribute was not recognized.
   */
  ParseResult<bool> trySetPresentationAttribute(EntityHandle handle, std::string_view name,
                                                std::string_view value) {
    auto result = properties_.parsePresentationAttribute(name, value, handle);
    refreshDeclarationHash();
    return result;
  }

private:
  void refreshDeclarationHash() {
    const auto key = properties_.localDeclarationKey();
    empty_ = key && key->empty();
    declarationHash_ = key ? std::optional(std::hash<std::string_view>{}(*key)) : std::nullopt;
  }

  PropertyRegistry properties_;
  std::optional<std::size_t> declarationHash_{std::hash<std::string_view>{}("")};
  bool empty_ = true;
};

}  // namespace donner::svg::components
