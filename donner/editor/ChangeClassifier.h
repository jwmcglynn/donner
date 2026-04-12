#pragma once
/// @file
///
/// Classifies a text edit as either an attribute-value change on a specific
/// element (fast path → `SetAttributeCommand`) or a structural change
/// (fallback → `ReplaceDocumentCommand`). This is the M5 gate: when
/// structured editing is enabled, the main loop calls `classifyTextChange`
/// instead of unconditionally emitting a `ReplaceDocumentCommand`.
///
/// The classifier is deliberately conservative — any edit it can't
/// conclusively identify as an in-attribute-value change falls through to
/// the structural-fallback path, which re-parses the entire document.

#include <optional>
#include <string_view>

#include "donner/editor/EditorCommand.h"
#include "donner/svg/SVGDocument.h"

namespace donner::editor {

/**
 * Result of classifying a text change.
 *
 * If `command` is set, the change was classified as an attribute-value
 * edit and the caller should emit `command` instead of a
 * `ReplaceDocumentCommand`. If `command` is `std::nullopt`, the change
 * is structural and the caller should fall back to a full re-parse.
 */
struct ClassifyResult {
  /// The command to emit if the change was classified as an attribute edit.
  /// `std::nullopt` means "structural — fall back to ReplaceDocument."
  std::optional<EditorCommand> command;
};

/**
 * Try to classify a text change as an in-attribute-value edit.
 *
 * Compares `oldSource` and `newSource` to find the changed byte range,
 * then checks whether that range falls entirely inside a single quoted
 * attribute value on a single element. If so, returns a
 * `SetAttributeCommand` for that element and attribute. Otherwise
 * returns an empty `ClassifyResult` (structural fallback).
 *
 * @param document The current SVG document (used to find elements by
 *   source offset).
 * @param oldSource The source text before the edit.
 * @param newSource The source text after the edit.
 * @return Classification result — either a `SetAttributeCommand` or
 *   `std::nullopt` for structural fallback.
 */
ClassifyResult classifyTextChange(svg::SVGDocument& document, std::string_view oldSource,
                                   std::string_view newSource);

}  // namespace donner::editor
