#pragma once
/// @file

#include <cstddef>
#include <istream>
#include <span>

#include "donner/base/parser/ParseResult.h"
#include "donner/svg/SVGDocument.h"

namespace donner::svg::parser {

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
   * Convert a string into a mutable vector<char> that is suitable for use with Donner's
   * XMLParser.
   */
  struct InputBuffer : std::vector<char> {
    /// Default constructor, for use with \ref loadFromStream.
    InputBuffer() = default;

    /**
     * Construct an input buffer from a string. Implicit so it enables passing a raw string into the
     * \ref XMLParser::ParseSVG function.
     *
     * Example:
     * ```
     * XMLParser::InputBuffer svgSource("<svg>...</svg>");
     * auto result = XMLParser::ParseSVG(svgSource);
     * ```
     *
     * @param str String to read from.
     */
    /* implicit */ InputBuffer(std::string_view str) {
      // Reserve enough space for the string, and an extra byte for the NUL ('\0') terminator if
      // required.
      const bool hasNul = str.ends_with('\0');
      reserve(str.size() + (hasNul ? 0 : 1));
      std::copy(str.begin(), str.end(), std::back_inserter(*this));
      if (!hasNul) {
        push_back('\0');
      }
    }

    /**
     * Append a string to the input buffer.
     *
     * @param str String to append.
     */
    void append(std::string_view str) {
      // Remove the null terminator if one is set.
      while (!empty() && back() == '\0') {
        pop_back();
      }

      // Reserve enough space for the string.
      reserve(size() + str.size());

      // Append the string.
      std::copy(str.begin(), str.end(), std::back_inserter(*this));
    }

    /**
     * Load the contents of an STL stream into the input buffer.
     *
     * Example:
     * ```
     * XMLParser::InputBuffer svgSource;
     * svgSource.loadFromStream(std::ifstream("example.svg"));
     * ```
     *
     * @param stream Input stream to read from.
     * @return The number of bytes read.
     */
    void loadFromStream(std::istream& stream) {
      stream.seekg(0, std::ios::end);
      const size_t fileLength = stream.tellg();
      stream.seekg(0);

      resize(fileLength + 1);
      stream.read(data(), static_cast<std::streamsize>(fileLength));
      data()[fileLength] = '\0';
    }
  };

  /**
   * Parses an SVG XML document (typically the contents of a .svg file).
   *
   * To reduce copying, the input buffer is modified to produce substrings, so it must be mutable
   * and end with a '\0'.
   *
   * @param source Mutable input data buffer.
   * @param[out] outWarnings If non-null, append warnings encountered to this vector.
   * @param options Options to modify the parsing behavior.
   * @return Parsed SVGDocument, or an error if a fatal error is encountered.
   */
  static ParseResult<SVGDocument> ParseSVG(InputBuffer& source,
                                           std::vector<ParseError>* outWarnings = nullptr,
                                           Options options = {}) noexcept;
};

}  // namespace donner::svg::parser
