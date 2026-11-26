#pragma once
/// @file

#include <optional>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/css/Stylesheet.h"

namespace donner::css::parser {

/**
 * Parse a CSS stylesheet into a list of selectors and their associated declarations.
 */
class StylesheetParser {
public:
  /**
   * Parse a CSS stylesheet into a list of selectors and their associated declarations, as well as
   * `@font-face` rules.
   *
   * Unconditional `@import` rules are retained in \ref Stylesheet::imports, in source order, with
   * their URLs exactly as authored; the parser never resolves or fetches them. Following the CSS
   * Cascade 5 grammar, an import is valid only before every other retained rule except `@layer`
   * statements that precede the first import (`@charset` is consumed first). A bare `layer`
   * modifier creates a distinct anonymous layer and `layer(name)` references the named layer.
   * Imports that appear later, have a malformed or empty target, or carry unsupported conditions
   * receive a bounded warning at the `@` token that never includes the target URL.
   *
   * CSS `@media` and `@supports` blocks are unsupported. Their complete blocks, including nested
   * styles and font faces, are ignored with a bounded warning at the outer `@` token.
   *
   * @param str Input stylesheet string.
   * @param warningSink Sink to collect warnings (e.g. invalid selectors).
   * @return Parsed stylesheet.
   */
  static Stylesheet Parse(std::string_view str, ParseWarningSink& warningSink);
  /// A single replacement in the original stylesheet text.
  struct TextEdit {
    std::size_t offset;
    std::size_t removedLength;
    std::size_t insertedLength;
  };

  /**
   * Reparse a bounded qualified-rule window, or return nullopt for a full parse.
   * @param old Fully parsed stylesheet before the edit.
   * @param before Original stylesheet text.
   * @param after Updated stylesheet text.
   * @param edit Byte replacement in the original stylesheet.
   * @param changed Optional rule-window output, set only on success.
   */
  static std::optional<Stylesheet> TryReparseTextEdit(const Stylesheet& old,
                                                      std::string_view before,
                                                      std::string_view after, TextEdit edit,
                                                      EditedRuleWindow* changed = nullptr);
};

}  // namespace donner::css::parser
