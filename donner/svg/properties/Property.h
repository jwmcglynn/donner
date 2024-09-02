#pragma once
/// @file

#include <optional>
#include <ostream>

#include "donner/css/Specificity.h"

namespace donner::svg {

/**
 * Defines how this property cascades between the parent and child elements.
 */
enum class PropertyCascade {
  None,         ///< Property does not inherit.
  Inherit,      ///< Property inherits unconditionally.
  PaintInherit  ///< Property inherits unless the child is instantiated as a paint server. This is
                ///< handled as a special case to prevent recursion for \ref xml_pattern.
};

/**
 * The current property state, which can be either set, not set, or a specific CSS keyword such as
 * "inherit", "initial", or "unset".
 */
enum class PropertyState {
  NotSet = 0,          ///< If the property has no value set.
  Set = 1,             ///< If the property has a value set.
  Inherit = 2,         ///< If the property's value is "inherit".
  ExplicitInitial = 3, /**< If the property's value is "initial", explicitly set by the user.
                        * Sets the property to its initial value with a specificity. */
  ExplicitUnset = 4,   /**< If the property's value is "unset", explicitly set by the user. Resolves
                        *   to either inherit or initial, depending on if the property is inheritable.
                        *
                        * @see https://www.w3.org/TR/css-cascade-3/#inherit-initial
                        */
};

/**
 * Options to control how inheritance is performed, to either inherit everything or conditionally
 * disable inheritance of paint servers.
 */
enum class PropertyInheritOptions {
  All,      ///< Inherit everything (default).
  NoPaint,  ///< Inherit everything except paint servers, for \ref xml_pattern elements.
};

/**
 * Callback function to get the initial value of a property.
 *
 * The function returns a `std::optional` to allow for properties that have no initial value.
 */
template <typename T>
using GetInitialFn = std::optional<T> (*)();

/**
 * Holds a CSS property, which has a name and value, and integrates with inheritance to allow
 * cascading values using the CSS model with specificity.
 *
 * @tparam T The type of the property value.
 * @tparam kCascade Determines how this property type participates in the cascade, to allow for
 *   specific property types to be excluded from inheritance.
 */
template <typename T, PropertyCascade kCascade = PropertyCascade::None>
struct Property {
  using Type = T;  ///< Type of the property value.

  /**
   * Property constructor, which is initially unset.
   *
   * @param name Name of the property, such as "color".
   * @param getInitialFn Function to get the initial value of the property.
   */
  Property(
      std::string_view name,
      GetInitialFn<T> getInitialFn = []() -> std::optional<T> { return std::nullopt; })
      : name(name), getInitialFn(getInitialFn) {}

  /**
   * Get the property value, without considering inheritance. Returns the initial value if
   * the property has not been set.
   *
   * @return The value if it is set, or the initial value if it is not. Returns `std::nullopt` if
   *   the property is none.
   */
  std::optional<T> get() const { return state == PropertyState::Set ? value : getInitialFn(); }

  /**
   * Gets the value of the property, requiring that the value is not std::nullopt.
   *
   * @return The value.
   */
  T getRequired() const {
    auto result = get();
    UTILS_RELEASE_ASSERT_MSG(result.has_value(), "Required property not set");
    return std::move(result.value());
  }

  /**
   * Gets a const-ref to the value, for accessing complex types without copying. Requires that \ref
   * hasValue() is true.
   *
   * @return const T& Reference to the value.
   */
  const T& getRequiredRef() const {
    UTILS_RELEASE_ASSERT_MSG(hasValue(), "Required property not set");
    return value.value();
  }

  /**
   * Set the property to a new value at the given specificity.
   *
   * @param newValue Value to set, or std::nullopt to set to an empty value.
   * @param newSpecificity Specificity to use.
   */
  void set(std::optional<T> newValue, css::Specificity newSpecificity) {
    value = std::move(newValue);
    state = PropertyState::Set;
    specificity = newSpecificity;
  }

  /**
   * Unset the current value and set the property to a specific state.
   *
   * @param newState New state to set.
   * @param newSpecificity Specificity to use.
   */
  void set(PropertyState newState, css::Specificity newSpecificity) {
    value.reset();
    state = newState;
    specificity = newSpecificity;
  }

  /**
   * Replace the current property's value with a new value at the current specificity.
   *
   * @param newValue Value to use to replace the existing one.
   */
  void substitute(std::optional<T> newValue) {
    value = std::move(newValue);
    state = PropertyState::Set;
  }

  /**
   * Clear the current property's value.
   */
  void clear() {
    value.reset();
    state = PropertyState::NotSet;
    specificity = css::Specificity();
  }

  /**
   * Inherit the property from the parent element, if the parent has the property set at a higher
   * specificity.
   *
   * Note that this typically inherits "backwards", taking a local property which may already have a
   * value and then overriding it if the parent has a more specific one.  This is not required, but
   * doing so is more efficient since we don't need to keep setting the property as the child
   * overrides each parent.
   *
   * @param parent Parent property to inherit into this one.
   * @param options Options to control how inheritance is performed, to conditionally disable
   *   inheritance.
   * @return Property with the resolved value after inheritance.
   */
  [[nodiscard]] Property<T, kCascade> inheritFrom(
      const Property<T, kCascade>& parent,
      PropertyInheritOptions options = PropertyInheritOptions::All) const {
    Property<T, kCascade> result = *this;

    if constexpr (kCascade == PropertyCascade::Inherit ||
                  kCascade == PropertyCascade::PaintInherit) {
      assert(parent.state != PropertyState::Inherit && "Parent should already be resolved");

      const bool isPaint = kCascade == PropertyCascade::PaintInherit;
      const bool canInherit = options == PropertyInheritOptions::All ||
                              (options == PropertyInheritOptions::NoPaint && !isPaint);

      if (parent.hasValue() && canInherit) {
        if (state == PropertyState::NotSet || state == PropertyState::Inherit ||
            state == PropertyState::ExplicitUnset) {
          // Inherit from parent.
          result.value = parent.get();
          // Keep current specificity.
          result.state = PropertyState::Set;
        } else if (parent.specificity > specificity) {
          // Inherit from parent, but with a lower specificity.
          result.value = parent.get();
          result.specificity = parent.specificity;
          result.state = PropertyState::Set;
        }
      }
    } else {
      // Inherit only if the state is Inherit.
      if (state == PropertyState::Inherit) {
        result.value = parent.get();
        // Keep current specificity.
        result.state = PropertyState::Set;
      }
    }

    return result;
  }

  /**
   * @return true if the property has any value set, including CSS built-in values.
   */
  bool hasValue() const { return state != PropertyState::NotSet; }

  std::string_view name;                        ///< Property name, such as "color".
  std::optional<T> value;                       ///< Property value, or `std::nullopt` if not set.
  PropertyState state = PropertyState::NotSet;  ///< Current state of the property, such as set or
                                                ///< inherited.
  css::Specificity specificity;  ///< Specificity of the property, used for inheritance.

  GetInitialFn<T>
      getInitialFn;  ///< Function which is called to get the initial value of the property.
};

/**
 * Ostream output operator, which outputs the current property value, how it was set (e.g. directly
 * set or inherited, see \ref PropertyState), and the property's specificity.
 *
 * Example output:
 * ```
 * color: Color(rgba(0, 255, 0, 255)) (set) @ Specificity(0, 0, 0)
 * ```
 *
 * @param os Output stream to write to.
 * @param property Property to output.
 */
template <typename T, PropertyCascade kCascade>
std::ostream& operator<<(std::ostream& os, const Property<T, kCascade>& property) {
  os << property.name << ":";

  if (property.state == PropertyState::Set) {
    if (property.value) {
      os << " " << *property.value;
    } else {
      os << " nullopt";
    }
  }

  switch (property.state) {
    case PropertyState::Set: os << " (set)"; break;
    case PropertyState::Inherit: os << " (inherit)"; break;
    case PropertyState::ExplicitInitial: os << " (explicit initial)"; break;
    case PropertyState::ExplicitUnset: os << " (explicit unset)"; break;
    case PropertyState::NotSet: os << " (not set)"; break;
  }

  if (property.state != PropertyState::NotSet) {
    os << " @ " << property.specificity;
  }

  return os;
}

}  // namespace donner::svg
