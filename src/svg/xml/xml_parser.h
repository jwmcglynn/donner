#pragma once
/// @file

#include <span>

#include "src/base/parser/parse_result.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

/**
 * Parse an SVG XML document.
 */
class XMLParser {
public:
  /**
   * Options to modify the parsing behavior.
   */
  struct Options {
    /// Default options.
    constexpr Options() {}

    /**
     * By default, the parser will ignore user-defined attributes (only presentation attributes will
     * be parsed), to optimize for performance. This behavior breaks some CSS matchers, which may
     * use user-defined attributes to control styling.
     *
     * For example:
     * ```svg
     * <svg>
     *   <style>
     *   rect[fill="red"] { fill: green; }
     *   rect[my-custom-attribute="value"] { stroke: green; }
     *   </style>
     *
     *   <rect x="10" y="20" width="30" height="40"
     *     my-custom-attribute="value"
     *     fill="red" stroke="red" />
     * </svg>
     * ```
     *
     * If user attributes are disabled (\ref disableUserAttributes is true), the above example will
     * only match the first rule, because `my-custom-attribute` will be ignored during parsing.
     *
     * To support rendering documents that use user-defined attributes, set this to false.
     */
    bool disableUserAttributes = true;
  };

  /**
   * Parses an SVG XML document (typically the contents of a .svg file).
   *
   * To reduce copying, the input buffer is modified to produce substrings, so it must be mutable
   * and end with a '\0'.
   *
   * @param str Mutable input data, which must be mutable and null-terminated.
   * @param[out] outWarnings If non-null, append warnings encountered to this vector.
   * @param options Options to modify the parsing behavior.
   * @return Parsed SVGDocument, or an error if a fatal error is encountered.
   */
  static ParseResult<SVGDocument> ParseSVG(std::span<char> str,
                                           std::vector<ParseError>* outWarnings = nullptr,
                                           Options options = {}) noexcept;
};

}  // namespace donner::svg
