#pragma once
/// @file

#include <cstddef>
#include <istream>

#include "donner/base/parser/ParseResult.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg::parser {

/**
 * Parse an SVG XML document.
 */
class SVGParser {
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
   * The input buffer does not need to be null-terminated, but if there are embedded null characters parsing will stop.
   *
   * @param source Input buffer containing the SVG XML document. Will not be modified.
   * @param[out] outWarnings If non-null, append warnings encountered to this vector.
   * @param options Options to modify the parsing behavior.
   * @param resourceLoader Resource loader to use for loading external resources.
   * @return Parsed SVGDocument, or an error if a fatal error is encountered.
   */
  static ParseResult<SVGDocument> ParseSVG(
      std::string_view source, std::vector<ParseError>* outWarnings = nullptr, Options options = {},
      std::unique_ptr<ResourceLoaderInterface> resourceLoader = nullptr) noexcept;
};

}  // namespace donner::svg::parser
