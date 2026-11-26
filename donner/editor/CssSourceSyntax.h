#pragma once
/// @file

#include <cstddef>
#include <functional>
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

/// Locate every CSS-bearing region in SVG/XML source, in source order.
/// @param source SVG/XML source bytes.
std::vector<CssSourceRegion> FindSvgCssSourceRegions(std::string_view source);

/// Visit the token colors for one CSS region, using absolute source byte offsets.
void ClassifySvgCssRegion(std::string_view source, const CssSourceRegion& region,
                          const std::function<void(CssColorSpan)>& paint);

}  // namespace donner::editor
