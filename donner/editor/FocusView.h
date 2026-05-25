#pragma once
/// @file

#include <optional>
#include <span>
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

/// View-only connector from a source reference to the referenced source target.
struct FocusReferenceLink {
  SourcePoint from;  ///< Location of the source reference token.
  SourcePoint to;    ///< Location of the referenced source target.

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

/// Focus information for a stylesheet rule under the source cursor.
struct StyleFocus {
  FocusPartition partition;                       ///< Source-pane partition for the matched rule.
  std::vector<svg::SVGElement> impactedElements;  ///< Elements matched by the selected rule.
};

/// Distinct same-document references related to the active selection.
struct ReferenceHighlightSummary {
  /// Elements directly referenced by the active selection.
  std::vector<svg::SVGElement> referencedElements;
  /// Elements that directly reference the active selection.
  std::vector<svg::SVGElement> referencingElements;

  /// Number of references represented by this summary.
  [[nodiscard]] std::size_t totalCount() const {
    return referencedElements.size() + referencingElements.size();
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

/**
 * Compute the source-pane focus partition for \p selectedElements.
 *
 * @param document Source-backed SVG document containing \p selectedElements.
 * @param selectedElements Elements whose source should remain fully visible.
 * @return Line partition for the source view, or an empty partition if source locations are absent.
 */
[[nodiscard]] FocusPartition ComputeFocusPartition(
    const svg::SVGDocument& document, std::span<const svg::SVGElement> selectedElements);

/**
 * Compute source-pane and canvas focus information for a stylesheet rule under \p sourceOffset.
 *
 * @param document Source-backed SVG document to inspect.
 * @param sourceOffset Byte offset in \p document source.
 * @return Style focus information, or \c std::nullopt when \p sourceOffset is not inside a
 * source-backed stylesheet rule.
 */
[[nodiscard]] std::optional<StyleFocus> ComputeStyleFocusAtSourceOffset(
    const svg::SVGDocument& document, std::size_t sourceOffset);

/**
 * Compute a focus partition for a stylesheet rule under \p sourceOffset.
 *
 * When \p sourceOffset is inside an author `<style>` rule, the resulting focus set shows the
 * matched rule, elements impacted by that selector, their ancestor context, and resources
 * referenced by the rule. Returns \c std::nullopt when the offset is not inside a source-backed
 * stylesheet rule.
 *
 * @param document Source-backed SVG document to inspect.
 * @param sourceOffset Byte offset in \p document source.
 */
[[nodiscard]] std::optional<FocusPartition> ComputeStyleFocusPartitionAtSourceOffset(
    const svg::SVGDocument& document, std::size_t sourceOffset);

/**
 * Compute direct forward and reverse reference elements for \p selectedElements.
 *
 * Forward references are same-document URL/href/CSS declaration refs from the selected elements.
 * Reverse references are elements whose URL/href/CSS declaration refs point at a selected resource.
 *
 * @param document Source-backed SVG document containing \p selectedElements.
 * @param selectedElements Elements whose related refs should be summarized.
 */
[[nodiscard]] ReferenceHighlightSummary ComputeReferenceHighlightSummary(
    const svg::SVGDocument& document, std::span<const svg::SVGElement> selectedElements);

}  // namespace donner::editor
