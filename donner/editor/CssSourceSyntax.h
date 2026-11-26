#pragma once
/// @file

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/TextBuffer.h"

namespace donner::editor {

/// How a CSS source region is embedded in SVG source.
enum class CssSourceRegionKind {
  Stylesheet,       ///< Text inside a style element.
  DeclarationList,  ///< The value of a style attribute.
  PropertyValue,    ///< The value of an SVG presentation attribute.
};

/// One CSS-bearing byte range found by the XML tokenizer.
struct CssSourceRegion {
  std::size_t startOffset = 0;                                 //!< Inclusive source byte offset.
  std::size_t endOffset = 0;                                   //!< Exclusive source byte offset.
  CssSourceRegionKind kind = CssSourceRegionKind::Stylesheet;  //!< CSS grammar at this range.
  std::string propertyName;  //!< Presentation property, empty for other region kinds.

  bool operator==(const CssSourceRegion&) const = default;
};

/// Syntax color for a half-open source byte range.
struct CssColorSpan {
  std::size_t startOffset = 0;             //!< Inclusive source byte offset.
  std::size_t endOffset = 0;               //!< Exclusive source byte offset.
  ColorIndex color = ColorIndex::Default;  //!< Existing editor palette class.

  bool operator==(const CssColorSpan&) const = default;
};

/// Regions found in the full SVG source and color spans for the requested affected range.
struct CssSourceClassification {
  std::vector<CssSourceRegion> regions;  //!< Every CSS region, in source order.
  std::vector<CssColorSpan> colors;      //!< Colors only for intersecting regions.
};

/**
 * Locate CSS with the XML tokenizer and classify selected regions with the CSS tokenizer.
 *
 * The XML scan is gap-free and always covers the full source so region boundaries remain correct
 * after malformed edits. Only a CSS region intersecting `[affectedStart, affectedEnd)` is passed
 * through the CSS tokenizer, allowing unchanged regions to keep their existing glyph colors.
 *
 * @param source SVG/XML source bytes.
 * @param affectedStart Inclusive start of the edited or requested byte range.
 * @param affectedEnd Exclusive end of the edited or requested byte range.
 */
CssSourceClassification ClassifySvgCssSource(
    std::string_view source, std::size_t affectedStart = 0,
    std::size_t affectedEnd = std::numeric_limits<std::size_t>::max());

}  // namespace donner::editor
