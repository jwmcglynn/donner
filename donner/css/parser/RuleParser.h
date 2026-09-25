#pragma once
/// @file

#include <cstddef>
#include <string_view>

#include "donner/css/Rule.h"

namespace donner::css::parser {

/**
 * Rule-related parsing routines, including parsing individual rules, lists of rules, and
 * stylesheets.
 */
class RuleParser {
public:
  /// Maximum rule-list entries emitted from one stylesheet or rule list.
  static constexpr std::size_t kMaximumRules = 4096;
  /// Maximum component values accepted while parsing one rule list.
  static constexpr std::size_t kMaximumComponentValues = 64 * 1024;

  /// Parsing work observed before a rule list finishes or reaches a limit.
  struct SecurityStats {
    std::size_t rules = 0;  ///< Rule-list entries emitted, including invalid placeholders.
    std::size_t componentValues = 0;  ///< Component values consumed so far.
    bool rejected = false;            ///< Whether a parsing limit rejected input.
  };

  /**
   * Parse a CSS stylesheet into a list of rules, per
   * https://www.w3.org/TR/css-syntax-3/#parse-stylesheet.
   *
   * NOTE: This does not parse selectors, in most cases you should use \ref StylesheetParser
   * instead which handles selectors and returns a \ref Stylesheet object to match against.
   *
   * @param str Input stylesheet string.
   * @param securityStats Optional destination for bounded parsing counters.
   * @return Parsed stylesheet as a list of rules.
   */
  static std::vector<Rule> ParseStylesheet(std::string_view str,
                                           SecurityStats* securityStats = nullptr);

  /**
   * Parse a list of rules, per https://www.w3.org/TR/css-syntax-3/#parse-list-of-rules
   *
   * @param str Input list of rules string.
   * @param securityStats Optional destination for bounded parsing counters.
   * @return Parsed list of rules.
   */
  static std::vector<Rule> ParseListOfRules(std::string_view str,
                                            SecurityStats* securityStats = nullptr);

  /**
   * Parse a rule, per https://www.w3.org/TR/css-syntax-3/#parse-rule
   *
   * @param str Input rule string.
   * @return Parsed rule, or std::nullopt if there was no rule.
   */
  static std::optional<Rule> ParseRule(std::string_view str);
};

}  // namespace donner::css::parser
