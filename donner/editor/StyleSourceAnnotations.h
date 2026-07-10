#pragma once
/// @file

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/FlashDecorations.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

/// Source contribution kind for editor-side cascade annotations.
enum class StyleContributionKind {
  StylesheetDeclaration,     ///< Declaration inside an authored `<style>` rule.
  InlineStyleDeclaration,    ///< Declaration inside an element `style=""` attribute.
  PresentationAttribute,     ///< SVG presentation attribute on an element.
  ReferenceResourceElement,  ///< Non-rendered resource element such as a gradient or filter.
};

/// One source-backed style contribution and its source-editor annotation metadata.
struct StyleSourceContribution {
  std::size_t id = 0;  ///< Stable id within a single annotation computation.
  SourceByteRange sourceRange;
  SourceByteRange chipRange;  ///< Source range whose end anchors the selector chip.
  std::string propertyName;
  StyleContributionKind kind = StyleContributionKind::StylesheetDeclaration;
  bool effective = false;  ///< True when this contribution wins for at least one element.
  bool showChip = false;   ///< True when a selector match-count chip should be shown.
  int matchedElementCount = 0;
  bool showOverflowMarker = false;  ///< True when a chip should show an overflow marker.
  std::vector<svg::SVGElement> matchedElements;
  std::string tooltip;          ///< Tooltip for the source range.
  std::string chipTooltip;      ///< Tooltip for the selector match-count chip.
  std::string overflowTooltip;  ///< Tooltip for the overflow marker.
};

/// Source annotations derived from CSS cascade analysis.
struct StyleSourceAnnotations {
  std::vector<StyleSourceContribution> contributions;
};

/// One annotation contribution detached from a document registry.
///
/// Background annotation work cannot return SVG element handles to the UI
/// thread. Stable writeback targets preserve chip-action identity without
/// sharing the parsed worker document.
struct DetachedStyleSourceContribution {
  StyleSourceContribution contribution;
  std::vector<std::size_t> matchedElementTargetIndices;
};

/// Registry-independent result of a source-only annotation computation.
struct DetachedStyleSourceAnnotations {
  bool valid = false;  ///< False when the source could not be parsed.
  /// Deduplicated targets shared by contribution match lists.
  std::vector<AttributeWritebackTarget> elementTargets;
  std::vector<DetachedStyleSourceContribution> contributions;
};

/**
 * Compute source-editor style annotations for \p document.
 *
 * This is intentionally read-only: it describes which source style contributions are selected by
 * the cascade but does not modify rendering, CSS state, or DOM state.
 *
 * @param document Source-backed SVG document to inspect.
 * @param source Source text currently synchronized with \p document.
 * @return Style contribution annotations for the text editor.
 */
[[nodiscard]] StyleSourceAnnotations ComputeStyleSourceAnnotations(svg::SVGDocument& document,
                                                                   std::string_view source);

/**
 * Parse source in an isolated document and compute registry-independent style annotations.
 *
 * Intended for background editor work. The returned element locators must be
 * resolved against the current UI document after its source revision is
 * revalidated.
 *
 * @param source Complete SVG source to parse and inspect.
 * @return Detached annotations, or a result with `valid == false` on parse failure.
 */
[[nodiscard]] DetachedStyleSourceAnnotations ComputeDetachedStyleSourceAnnotations(
    std::string source);

}  // namespace donner::editor
