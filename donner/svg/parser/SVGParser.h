#pragma once
/// @file

#include <cstddef>
#include <istream>

#include "donner/base/ParseResult.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/svg/SVGDocument.h"

namespace donner::svg::parser {

/**
 * Parse an SVG XML document.
 *
 * Elements outside the SVG namespace are retained in the tree as unknown elements rather than
 * detached, so whole-tree consumers (attribute selectors, export checks, source projection) see
 * a complete document. They are marked non-rendering: neither a foreign element nor its subtree
 * paints, matching how conforming SVG consumers treat foreign content. A retained foreign
 * subtree materializes one entity per element and counts against \ref Options::maximumTreeNodes
 * and \ref Options::maximumTreeDepth like any other content, and reports one
 * unsupported-namespace warning at the top of the subtree.
 */
class SVGParser {
public:
  /// Default maximum number of source or expanded SVG bytes accepted from untrusted input.
  static constexpr size_t kDefaultMaximumInputSize = 16 * 1024 * 1024;

  /// Default retained dynamic payload envelope for one untrusted SVG document.
  static constexpr size_t kDefaultMaximumParsedPayloadSize = 64 * 1024 * 1024;

  /// Default maximum direct text/CDATA chunks projected into one SVG element.
  static constexpr size_t kDefaultMaximumContentProjectionChunks = 4096;

  /// Default maximum XML tree nodes visited while converting a document into SVG components.
  static constexpr size_t kDefaultMaximumTreeNodes = 8 * 1024;

  /// Default maximum SVG element depth visited during XML-to-SVG conversion.
  static constexpr size_t kDefaultMaximumTreeDepth = 256;

  /**
   * Options to modify the parsing behavior.
   */
  struct Options {
    /// Default options.
    constexpr Options() {}

    /**
     * By default, the parser retains user-defined attributes on the parsed tree so the tree
     * stays complete for whole-tree consumers (attribute selectors, export checks) and CSS
     * matchers that key off custom attributes keep working.
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
     * Both rules match by default. If user attributes are disabled (\ref disableUserAttributes
     * is true), only the first rule matches, because `my-custom-attribute` is omitted
     * during parsing.
     *
     * Set this to true only to optimize for performance when custom attributes are known
     * to be irrelevant; whole-tree attribute checks then cannot see them.
     */
    bool disableUserAttributes = false;

    /**
     * Enable experimental or incomplete features. When true, element types that declare
     * `static constexpr bool IsExperimental = true` are parsed as their concrete type; when false,
     * they are treated as unknown elements.
     *
     * The SVG animation elements (`animate`, `animateTransform`, and `set`) are currently
     * experimental and require this option. Text and filters are enabled independently.
     */
    bool enableExperimental = false;

    /**
     * Parse as inlined SVG content. This will treat the input as a fragment of SVG content, rather
     * than a full SVG document. This is useful for parsing SVG content embedded in HTML or other
     * XML documents.
     *
     * This enables the following shorthand without the `xmlns` attribute:
     * ```xml
     * <svg><rect /></svg>
     * ```
     *
     * Instead of the full document:
     * ```xml
     * <svg xmlns="http://www.w3.org/2000/svg"><rect /></svg>
     * ```
     */
    bool parseAsInlineSVG = false;

    /**
     * Maximum source size accepted by ParseSVG. For SVGZ input the same limit applies to both the
     * compressed and expanded forms, preventing decompression bombs.
     */
    size_t maximumInputSize = kDefaultMaximumInputSize;

    /**
     * Maximum estimated bytes retained by parsed lists, stylesheets, and deferred geometry.
     * This is aggregate across the document, rather than a per-attribute limit.
     */
    size_t maximumParsedPayloadSize = kDefaultMaximumParsedPayloadSize;

    /** Maximum direct text/CDATA chunks joined into one text, style, or descriptive element. */
    size_t maximumContentProjectionChunks = kDefaultMaximumContentProjectionChunks;

    /** Maximum XML tree nodes visited while converting either parser entry point. */
    size_t maximumTreeNodes = kDefaultMaximumTreeNodes;

    /** Maximum nested SVG element depth converted from an XML document. */
    size_t maximumTreeDepth = kDefaultMaximumTreeDepth;
  };

  /**
   * Parses an SVG XML document from a string (typically the contents of a .svg file).
   *
   * The input buffer does not need to be null-terminated, but if there are embedded null characters
   * parsing will stop.
   *
   * @param source Input buffer containing the SVG XML document. Will not be modified.
   * @param warningSink Sink to collect warnings encountered during parsing.
   * @param options Options to modify the parsing behavior.
   * @param settings Document settings, including the resource loader and processing mode.
   * @return Parsed SVGDocument, or an error if a fatal error is encountered.
   */
  static ParseResult<SVGDocument> ParseSVG(std::string_view source, ParseWarningSink& warningSink,
                                           Options options = {},
                                           SVGDocument::Settings settings = {}) noexcept;

  /**
   * Parses an SVG XML document from an XML document tree.
   *
   * @param xmlDocument XML document to parse.
   * @param warningSink Sink to collect warnings encountered during parsing.
   * @param options Options to modify the parsing behavior.
   * @param settings Document settings, including the resource loader and processing mode.
   * @return Parsed SVGDocument, or an error if a fatal error is encountered.
   */
  static ParseResult<SVGDocument> ParseXMLDocument(xml::XMLDocument&& xmlDocument,
                                                   ParseWarningSink& warningSink,
                                                   Options options = {},
                                                   SVGDocument::Settings settings = {}) noexcept;
};

}  // namespace donner::svg::parser
