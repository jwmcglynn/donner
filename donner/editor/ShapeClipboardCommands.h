#pragma once
/// @file
///
/// Pure shape-clipboard command helpers, decoupled from ImGui and from the
/// editor shell so they can be unit-tested headlessly (see
/// `donner/editor/tests/ShapeClipboard_tests.cc`).
///
/// These functions implement the structural core of Cut/Copy/Paste/Paste-in-Front:
///
/// - `copySelectionToPayload` serializes the selected element subtrees to a
///   `ShapeClipboardPayload`. Each subtree's SVG text comes from the document's
///   own source store substring (`SVGDocument::source()` + the element's XML
///   node source range), which keeps the copied bytes byte-for-byte identical to
///   what the user authored.
/// - `preparePaste` parses a payload, detects id collisions against the current
///   document, deterministically renames colliding ids (`_pasted`, `_pasted2`,
///   …), rewrites internal `href` / `xlink:href` / `url(#id)` references, and
///   produces the merged full-document SVG source plus the ids that should be
///   selected after the paste. It fails (returning an error, leaving its inputs
///   untouched) when a reference cannot be repaired safely.
///
/// The editor wires the merged source through `EditorCommand::Kind::PasteShapes`
/// / `Kind::CutShapes` so the resulting reparse is a single undo step that keeps
/// the source pane coherent.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/ShapeClipboardPayload.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

/// Build a clipboard payload from the given selection (in paint order).
///
/// Serializes each selected element subtree from the document source store. For
/// in-editor elements with no source mapping, the element is skipped (a future
/// temporary-doc round-trip can fill this gap); if no element can be serialized,
/// returns std::nullopt.
///
/// @param document Source document (provides `source()` and bounds).
/// @param selection Selected elements, in the order they should be serialized.
/// @returns Payload, or std::nullopt if the selection is empty / unserializable.
[[nodiscard]] std::optional<ShapeClipboardPayload> copySelectionToPayload(
    const svg::SVGDocument& document, const std::vector<svg::SVGElement>& selection);

/// Where a paste should insert its fragment.
enum class PastePlacement : std::uint8_t {
  /// Default paste: append inside the root `<svg>` (or selected group) and offset
  /// the fragment by `translate(20,20)` so the new shapes are visible.
  EndOfRootOffset,
  /// Paste-in-Front: no offset; insert above the source elements in paint order
  /// when they still exist, otherwise at the end of the root.
  InFrontNoOffset,
};

/// Result of preparing a paste.
struct PreparePasteResult {
  /// Whether the paste can be applied. When false, `error` describes why and
  /// `mergedSource` is empty - the caller must not mutate the document.
  bool ok = false;

  /// Human-readable failure reason for the user when `ok` is false.
  std::string error;

  /// Full merged SVG document source to reparse when `ok` is true.
  std::string mergedSource;

  /// Ids of the pasted top-level elements (after id repair), in paint order.
  /// The caller selects these after the reparse. Always populated when `ok`.
  std::vector<std::string> pastedElementIds;
};

/// Prepare a paste of \p payload into \p document.
///
/// Does not mutate \p document; it computes the merged source string the editor
/// should reparse. See file comment for id/reference repair semantics.
///
/// @param document Destination document.
/// @param payload Parsed clipboard payload.
/// @param placement Default-offset vs. in-front placement.
/// @param selectedGroup Optional single selected group/svg container to paste
///   into instead of the root (default paste only).
[[nodiscard]] PreparePasteResult preparePaste(
    const svg::SVGDocument& document, const ShapeClipboardPayload& payload,
    PastePlacement placement, const std::optional<svg::SVGElement>& selectedGroup = std::nullopt);

}  // namespace donner::editor
