#pragma once

#include <span>

#include "src/base/parser/parse_result.h"
#include "src/css/color.h"
#include "src/css/declaration.h"
#include "src/css/stylesheet.h"
#include "src/svg/properties/paint_server.h"

namespace donner::svg {

class PropertyRegistry;

enum class PropertyState {
  NotSet = 0,
  Set = 1,             //!< If the property has a value set.
  Inherit = 2,         //!< If the property's value is "inherit".
  ExplicitInitial = 3, /**< If the property's value is "initial", explicitly set by the user. Sets
                        *   the property to its initial value with a specificity. */
  ExplicitUnset = 4,   /**< If the property's value is "unset", explicitly set by the user. Resolves
                        *   to either inherit or initial, depending on if the property is inheritable.
                        *
                        * @see https://www.w3.org/TR/css-cascade-3/#inherit-initial
                        */
};

enum class PropertyCascade { None, Inherit };

struct PropertyParseFnParams {
  std::span<const css::ComponentValue> components;
  PropertyState explicitState = PropertyState::NotSet;
  css::Specificity specificity;
  /// For presentation attributes, values may be unitless, in which case they the spec says they are
  /// specified in "user units". See https://www.w3.org/TR/SVG2/types.html#syntax.
  bool allowUserUnits = false;
};

using PropertyParseFn = std::optional<ParseError> (*)(PropertyRegistry& registry,
                                                      const PropertyParseFnParams& params);

class PropertyRegistry {
public:
  template <typename T>
  using GetInitialFn = std::optional<T> (*)();

  template <typename T, PropertyCascade kCascade = PropertyCascade::None>
  struct Property {
    Property(GetInitialFn<T> getInitialFn = []() -> std::optional<T> { return std::nullopt; })
        : getInitialFn(getInitialFn) {}

    /**
     * Get the property value, without considering inheritance. Returns the initial value if the
     * property has not been set.
     */
    std::optional<T> get() const { return state == PropertyState::Set ? value : getInitialFn(); }
    void set(std::optional<T> newValue, css::Specificity newSpecificity) {
      value = std::move(newValue);
      state = PropertyState::Set;
      specificity = newSpecificity;
    }

    void set(PropertyState newState, css::Specificity newSpecificity) {
      value.reset();
      state = newState;
      specificity = newSpecificity;
    }

    [[nodiscard]] Property<T, kCascade> inheritFrom(const Property<T, kCascade>& parent) const {
      Property<T, kCascade> result = *this;

      if constexpr (kCascade == PropertyCascade::Inherit) {
        assert(parent.state != PropertyState::Inherit && "Parent should already be resolved");

        if (state == PropertyState::NotSet || state == PropertyState::Inherit ||
            state == PropertyState::ExplicitUnset) {
          // Inherit from parent.
          result.value = parent.get();
          // Keep current specificity.
          result.state = PropertyState::Set;
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

    std::optional<T> value;
    PropertyState state = PropertyState::NotSet;
    css::Specificity specificity;

    GetInitialFn<T> getInitialFn;
  };

  Property<css::Color, PropertyCascade::Inherit> color;
  Property<PaintServer, PropertyCascade::Inherit> fill{[]() -> std::optional<PaintServer> {
    return PaintServer::Solid(css::Color(css::RGBA::RGB(0, 0, 0)));
  }};
  Property<PaintServer, PropertyCascade::Inherit> stroke{
      []() -> std::optional<PaintServer> { return PaintServer::None(); }};
  Property<Lengthd, PropertyCascade::Inherit> strokeWidth{
      []() -> std::optional<Lengthd> { return Lengthd(1, Lengthd::Unit::None); }};

  /**
   * Inherit the value of each element in the stylesheet.
   */
  [[nodiscard]] PropertyRegistry inheritFrom(const PropertyRegistry& parent) const {
    PropertyRegistry result;
    result.color = color.inheritFrom(parent.color);
    result.fill = fill.inheritFrom(parent.fill);
    result.stroke = stroke.inheritFrom(parent.stroke);
    result.strokeWidth = strokeWidth.inheritFrom(parent.strokeWidth);

    return result;
  }

  /**
   * Parse a single declaration, adding it to the property registry.
   *
   * @param declaration Declaration to parse.
   * @param specificity Specificity of the declaration.
   * @return Error if the declaration had errors parsing or the property is not supported.
   */
  std::optional<ParseError> parseProperty(const css::Declaration& declaration,
                                          css::Specificity specificity);

  /**
   * Parse a HTML/SVG style attribute, corresponding to a CSS <declaration-list>, ignoring any parse
   * errors or unsupported properties.
   *
   * @param str Input string.
   */
  void parseStyle(std::string_view str);

  /**
   * Parse a presentation attribute, which can contain a CSS value.
   *
   * @see https://www.w3.org/TR/SVG2/styling.html#PresentationAttributes
   * @param name Name of the attribute.
   * @param value Value of the attribute, parsed as a CSS value.
   * @return true if the attribute name was supported.
   */
  bool parsePresentationAttribute(std::string_view name, std::string_view value);
};

}  // namespace donner::svg
