#pragma once

#include <cassert>   // For assert
#include <cctype>    // For isspace
#include <concepts>  // For std::invocable
#include <optional>
#include <string_view>

#include "donner/base/ParseError.h"

namespace donner::svg::parser {

/**
 * Concept requiring a type to be invocable with a \c std::string_view and return void.
 * Used to constrain the callback function for ListParser.
 */
template <typename F>
concept ListParserItemCallback = std::invocable<F, std::string_view> &&
                                 std::same_as<std::invoke_result_t<F, std::string_view>, void>;

/**
 * Parses a list of values conforming to the SVG comma-or-space list syntax.
 *
 * This parser adheres to the rules for SVG lists, which allow items to be
 * separated by commas, whitespace, or a mix of both. It calls a provided
 * function for each individual item found in the list.
 *
 * Grammar allows:
 * - `item1, item2 item3 , item4`
 * - Whitespace around commas is ignored.
 * - Multiple spaces between items are ignored.
 *
 * Invalid syntax (returns false):
 * - Empty items (e.g., `item1,,item2`)
 * - Trailing commas (e.g., `item1, item2,`)
 * - Leading commas (e.g., `, item1`)
 *
 * Example Usage:
 * ```cpp
 * std::vector<std::string_view> items;
 * bool success = ListParser::Parse("item1 item2, item3", [&](std::string_view item) {
 *   items.push_back(item);
 * });
 * // success will be true, items will contain {"item1", "item2", "item3"}
 * ```
 */
class ListParser {
public:
  /**
   * Parses the SVG comma-or-space separated list from the given \c std::string_view.
   *
   * @tparam Fn Type of the function to call for each list item, with signature
   * `void(std::string_view)`.
   * @param value The string_view containing the list to parse.
   * @param fn The function to call for each parsed item.
   * @return std::nullopt on success, or a ParseError containing the reason and position of the
   * error on failure.
   */
  template <ListParserItemCallback Fn>
  static std::optional<ParseError> Parse(std::string_view value, Fn fn) {
    const size_t length = value.size();
    size_t i = 0;
    bool expectItem = true;               // Start expecting an item
    bool processedNonWhitespace = false;  // Track if we've seen non-whitespace

    while (i < length) {
      SkipWhitespace(value, i);
      if (i == length) {
        break;  // Reached end after whitespace
      }

      processedNonWhitespace = true;  // Found a non-whitespace char

      if (value[i] == ',') {
        if (expectItem) {
          // Found a comma when an item was expected (e.g., ", item" at start, or "item1,,item2")
          return ParseError{"Unexpected comma, expected list item", FileOffset::Offset(i)};
        }

        i++;                // Consume comma
        expectItem = true;  // Expect an item after a comma
        continue;           // Go back to skip whitespace before the next potential item
      }

      // If we reach here, we expect an item (not a comma or whitespace)
      const size_t start = i;
      while (i < length && !std::isspace(static_cast<unsigned char>(value[i])) && value[i] != ',') {
        i++;
      }

      // It should not be possible for start == i here, because we checked for
      // whitespace/comma/end-of-string before.
      assert(start != i && "Parser did not advance, potential infinite loop.");

      fn(value.substr(start, i - start));

      // After an item, expect a comma or end-of-string (or whitespace then comma/end)
      expectItem = false;
    }

    // Valid end states:
    // 1. Reached end of string (i == length)
    // 2. We were not expecting an item (last thing was an item, or it was an empty valid string)

    assert(i == length && "Internal parser error: expected end of string");

    if (expectItem && processedNonWhitespace) {
      // This means we had a trailing comma after processing something.
      // The error occurred just before the end, where the comma was.
      return ParseError{"Unexpected trailing comma", FileOffset::Offset(i > 0 ? i - 1 : 0)};
    }

    return std::nullopt;  // Success
  }

private:
  /**
   * Skips whitespace characters in a string_view starting from a given index.
   *
   * @param value The string view to process.
   * @param i Reference to the current index, will be updated.
   */
  static inline void SkipWhitespace(std::string_view value, size_t& i) {
    while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) {
      i++;
    }
  }
};

}  // namespace donner::svg::parser
