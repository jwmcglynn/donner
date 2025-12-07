#pragma once
/// @file

#include <span>
#include <string_view>

#include "donner/base/ParseResult.h"
#include "donner/css/Color.h"
#include "donner/css/ColorProfile.h"
#include "donner/css/ComponentValue.h"

namespace donner::css::parser {

/**
 * Parse a CSS color, either from a string or the CSS intermediate representation, a list of
 * ComponentValues.
 */
class ColorParser {
public:
  /// Options for color parsing.
  struct Options {
    /// Optional registry containing custom `@color-profile` bindings.
    const ColorProfileRegistry* profileRegistry = nullptr;
  };

  /**
   * Parse a CSS color, per https://www.w3.org/TR/2021/WD-css-color-4-20210601/
   *
   * Supports named colors, hex colors, and color functions such as rgb().
   *
   * @param components List of component values from the color declaration.
   * @return Parsed color.
   */
  static ParseResult<Color> Parse(std::span<const ComponentValue> components);
  static ParseResult<Color> Parse(std::span<const ComponentValue> components,
                                  const Options& options);

  /**
   * Parse a CSS color from a string, per https://www.w3.org/TR/2021/WD-css-color-4-20210601/
   *
   * Supports named colors, hex colors, and color functions such as rgb().
   *
   * @param str String that can be parsed into a list color declaration.
   * @return Parsed color.
   */
  static ParseResult<Color> ParseString(std::string_view str);
  static ParseResult<Color> ParseString(std::string_view str, const Options& options);
};

}  // namespace donner::css::parser
