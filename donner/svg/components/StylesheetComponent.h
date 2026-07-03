#pragma once
/// @file

#include <optional>
#include <span>
#include <vector>

#include "donner/base/FileOffset.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Utils.h"
#include "donner/css/Stylesheet.h"

namespace donner::svg::components {

/// Maps a contiguous span of parsed CSS text back to the SVG document source.
struct StylesheetSourceMapSegment {
  std::size_t cssStartOffset = 0;  ///< First local CSS byte covered by this segment.
  std::size_t cssEndOffset = 0;    ///< First local CSS byte after this segment.
  FileOffset documentStartOffset{
      FileOffset::Offset(0)};  ///< Document source offset for \ref cssStartOffset.

  /// Equality operator.
  bool operator==(const StylesheetSourceMapSegment& other) const = default;
};

/// Source map for stylesheet text produced by concatenating `<style>` text/CDATA children.
class StylesheetSourceMap {
public:
  /**
   * Add one local-CSS to document-source mapping segment.
   *
   * @param cssStartOffset First local CSS byte covered by this segment.
   * @param cssEndOffset First local CSS byte after this segment.
   * @param documentStartOffset Document source offset for \p cssStartOffset.
   */
  void addSegment(std::size_t cssStartOffset, std::size_t cssEndOffset,
                  FileOffset documentStartOffset);

  /**
   * Return true when no local CSS offsets can be mapped to document source.
   */
  bool empty() const { return segments_.empty(); }

  /**
   * Map a local CSS source range back to the SVG document source.
   *
   * @param localRange Range in the parsed CSS string.
   * @return Document source range, or \c std::nullopt when either endpoint is unmappable.
   */
  std::optional<SourceRange> mapToDocumentSource(const SourceRange& localRange) const;

  /**
   * Map a document source offset back to this stylesheet's local CSS offset.
   *
   * @param documentOffset Offset in the SVG document source.
   * @return Local CSS offset, or \c std::nullopt if \p documentOffset is outside this stylesheet.
   */
  std::optional<std::size_t> mapToLocalCssOffset(std::size_t documentOffset) const;

  /**
   * Get the stored mapping segments.
   */
  std::span<const StylesheetSourceMapSegment> segments() const UTILS_LIFETIME_BOUND {
    return segments_;
  }

private:
  std::optional<FileOffset> mapOffset(const FileOffset& localOffset) const;

  std::vector<StylesheetSourceMapSegment> segments_;
};

/**
 * Data for a \ref xml_style element.
 *
 * See https://www.w3.org/TR/SVG2/styling.html#StyleElement
 */
struct StylesheetComponent {
  css::Stylesheet stylesheet;     ///< The parsed stylesheet from the \ref xml_style element.
  RcString text;                  ///< The raw CSS text the \ref stylesheet was parsed from.
  RcString type;                  ///< The type attribute of the \ref xml_style element.
  StylesheetSourceMap sourceMap;  ///< Optional mapping from CSS offsets to SVG document source.

  /// True if this is a user agent stylesheet, which is defined by the SVG specification and applied
  /// by default by the document. If set, this component is attached to an \ref xml_svg element
  /// instead. The stylesheet within is applied at lower priority, and should be considered a
  /// constant for Donner (it can be overridden, but the base style cannot be changed by the user).
  bool isUserAgentStylesheet = false;

  /**
   * Returns true if the \ref xml_style element has either no `type` attribute, or if it has been
   * manually set to "text/css".
   */
  bool isCssType() const { return type.empty() || type.equalsIgnoreCase("text/css"); }

  /**
   * Parse the contents of the \ref xml_style element.
   *
   * @param str The contents of the \ref xml_style element.
   */
  void parseStylesheet(const RcStringOrRef& str);

  /**
   * Parse the contents of the \ref xml_style element with source provenance.
   *
   * @param str The contents of the \ref xml_style element.
   * @param sourceMap Source map for local CSS offsets in \p str.
   */
  void parseStylesheet(const RcStringOrRef& str, StylesheetSourceMap sourceMap);
};

}  // namespace donner::svg::components
