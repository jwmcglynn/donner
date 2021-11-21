#pragma once

#include <span>

#include "src/base/parser/parse_result.h"
#include "src/css/color.h"
#include "src/css/declaration.h"
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

struct PropertyParseFnParams {
  std::span<const css::ComponentValue> components;
  PropertyState explicitState = PropertyState::NotSet;
  uint32_t specificity = 0;
};

using PropertyParseFn = std::optional<ParseError> (*)(PropertyRegistry& registry,
                                                      const PropertyParseFnParams& params);

class PropertyRegistry {
public:
  template <typename T>
  using GetInitialFn = std::optional<T> (*)();

  template <typename T>
  struct Property {
    Property(GetInitialFn<T> getInitialFn = []() -> std::optional<T> { return std::nullopt; })
        : getInitialFn(getInitialFn) {}

    /**
     * Get the property value, without considering inheritance. Returns the initial value if the
     * property has not been set.
     */
    std::optional<T> get() { return state == PropertyState::Set ? value : getInitialFn(); }
    void set(std::optional<T> newValue, uint32_t newSpecificity) {
      value = std::move(newValue);
      state = PropertyState::Set;
      specificity = newSpecificity;
    }

    void set(PropertyState newState, uint32_t newSpecificity) {
      value.reset();
      state = newState;
      specificity = newSpecificity;
    }

    /**
     * @return true if the property has any value set, including CSS built-in values.
     */
    bool hasValue() const { return state != PropertyState::NotSet; }

    std::optional<T> value;
    PropertyState state = PropertyState::NotSet;
    uint32_t specificity = 0;

    GetInitialFn<T> getInitialFn;
  };

  Property<css::Color> color;
  Property<PaintServer> fill{[]() -> std::optional<PaintServer> {
    return PaintServer::Solid(css::Color(css::RGBA::RGB(0, 0, 0)));
  }};
  Property<PaintServer> stroke{[]() -> std::optional<PaintServer> { return PaintServer::None(); }};

  /**
   * Parse a single declaration, adding it to the property registry.
   *
   * @param declaration Declaration to parse.
   * @param specificity Specificity of the declaration.
   * @return Error if the declaration had errors parsing or the property is not supported.
   */
  std::optional<ParseError> parseProperty(const css::Declaration& declaration,
                                          uint32_t specificity = 0);

  /**
   * Parse a HTML/SVG style attribute, corresponding to a CSS <declaration-list>, ignoring any parse
   * errors or unsupported properties.
   *
   * @param str Input string.
   */
  void parseStyle(std::string_view str);
};

}  // namespace donner::svg
