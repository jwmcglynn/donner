#pragma once
/// @file

#include <vector>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

/// Half-open line range in zero-based editor line coordinates.
struct LineRange {
  int startLine = 0;  ///< First included line.
  int endLine = 0;    ///< First excluded line.

  /// Equality operator.
  bool operator==(const LineRange& other) const = default;
};

/// Zero-based source coordinate in logical editor lines and columns.
struct SourcePoint {
  int line = 0;    ///< Source line.
  int column = 0;  ///< Source column.

  /// Equality operator.
  bool operator==(const SourcePoint& other) const = default;
};

/// View-only connector from an attribute reference to the referenced element.
struct FocusReferenceLink {
  SourcePoint from;  ///< Location of the fragment reference token.
  SourcePoint to;    ///< Opening `<` of the referenced element.

  /// Equality operator.
  bool operator==(const FocusReferenceLink& other) const = default;
};

/// View-only source-pane partition for selected-element focus mode.
struct FocusPartition {
  std::vector<LineRange> fullColor;                ///< Selected element's source span.
  std::vector<LineRange> dimmed;                   ///< Ancestor opening/closing tag context.
  std::vector<LineRange> hidden;                   ///< Lines hidden by focus mode.
  std::vector<FocusReferenceLink> referenceLinks;  ///< Attribute-reference connectors.

  /// Return true when the partition should be treated as "show everything".
  [[nodiscard]] bool empty() const {
    return fullColor.empty() && dimmed.empty() && hidden.empty() && referenceLinks.empty();
  }
};

/**
 * Compute the source-pane focus partition for \p selected.
 *
 * @param document Source-backed SVG document containing \p selected.
 * @param selected Element whose source should remain fully visible.
 * @return Line partition for the source view, or an empty partition if source locations are absent.
 */
[[nodiscard]] FocusPartition ComputeFocusPartition(const svg::SVGDocument& document,
                                                   const svg::SVGElement& selected);

}  // namespace donner::editor
