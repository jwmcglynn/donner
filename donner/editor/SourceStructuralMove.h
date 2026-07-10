#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "donner/editor/FlashDecorations.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

class EditorApp;

/// Validation result for a source-initiated structural element move.
enum class SourceStructuralMoveStatus : std::uint8_t {
  Ready,
  DocumentUnavailable,
  StaleRevision,
  RootElement,
  Locked,
  InvalidParent,
  InvalidReference,
  Cycle,
  NoSourceRange,
  NoChange,
  Rejected,
};

/// Revision-bound, DOM-shaped move prepared from a source drag target.
struct SourceStructuralMovePlan {
  svg::SVGElement element;
  svg::SVGElement parent;
  std::optional<svg::SVGElement> referenceElement;
  SourceByteRange elementRange;
  std::size_t insertionOffset = 0;
  std::uint64_t expectedDocumentGeneration = 0;
  std::uint64_t expectedFrameVersion = 0;
  std::uint64_t expectedSourceHash = 0;
};

/// Result of validating and building a structural move plan.
struct SourceStructuralMoveEvaluation {
  SourceStructuralMoveStatus status = SourceStructuralMoveStatus::Rejected;
  std::optional<SourceStructuralMovePlan> plan;
};

/**
 * Build a structural move plan without mutating the document.
 *
 * @param app Current editor document owner.
 * @param source Editable source corresponding exactly to the current DOM.
 * @param element Complete source-backed element being dragged.
 * @param parent Destination container.
 * @param referenceElement Destination sibling, or \c std::nullopt to append.
 */
[[nodiscard]] SourceStructuralMoveEvaluation BuildSourceStructuralMovePlan(
    EditorApp& app, std::string_view source, svg::SVGElement element, svg::SVGElement parent,
    std::optional<svg::SVGElement> referenceElement);

/**
 * Commit a previously validated plan if its source and document revisions are still current.
 *
 * @param app Current editor document owner.
 * @param plan Plan returned by \ref BuildSourceStructuralMovePlan.
 * @param source Current editable source.
 * @return \ref SourceStructuralMoveStatus::Ready when the DOM move was queued.
 */
[[nodiscard]] SourceStructuralMoveStatus CommitSourceStructuralMove(
    EditorApp& app, const SourceStructuralMovePlan& plan, std::string_view source);

/// Human-readable reason suitable for a disabled drop-target tooltip.
[[nodiscard]] std::string_view SourceStructuralMoveStatusMessage(SourceStructuralMoveStatus status);

}  // namespace donner::editor
