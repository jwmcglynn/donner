#pragma once

#include <span>

#include "src/base/parser/parse_result.h"
#include "src/css/color.h"
#include "src/css/declaration.h"
#include "src/svg/properties/paint_server.h"

namespace donner::svg {

class PropertyRegistry;

struct PropertyParseFnParams {
  std::span<const css::ComponentValue> components;
  bool inherit = false;
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

    std::optional<T> get() { return isSet ? value : getInitialFn(); }
    void set(std::optional<T> newValue, uint32_t newSpecificity) {
      isSet = true;
      value = std::move(newValue);
      specificity = newSpecificity;
    }

    void reset(uint32_t newSpecificity = 0) {
      isSet = false;
      value.reset();
      specificity = newSpecificity;
    }

    std::optional<T> value;
    uint32_t specificity = 0;
    bool isSet = false;

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
