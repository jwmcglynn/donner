#pragma once

#include "src/base/parser/parse_result.h"
#include "src/css/color.h"
#include "src/css/declaration.h"
#include "src/svg/properties/paint_server.h"

namespace donner::svg {

class PropertyRegistry;

using ParseFunction = std::optional<ParseError> (*)(PropertyRegistry& registry,
                                                    const css::Declaration& declaration);

class PropertyRegistry {
public:
  std::optional<css::Color> color;
  std::optional<PaintServer> fill;

  /**
   * Parse a single declaration, adding it to the property registry.
   *
   * @param declaration Declaration to parse.
   * @return Error if the declaration had errors parsing or the property is not supported.
   */
  std::optional<ParseError> parseProperty(const css::Declaration& declaration);

  /**
   * Parse a HTML/SVG style attribute, corresponding to a CSS <declaration-list>, ignoring any parse
   * errors or unsupported properties.
   *
   * @param str Input string.
   */
  void parseStyle(std::string_view str);
};

}  // namespace donner::svg