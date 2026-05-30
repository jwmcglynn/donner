#pragma once
/// @file
///
/// `ShapeClipboardPayload` is the structured representation of a shape-clipboard
/// copy. The editor serializes the selected element subtrees to SVG fragment
/// text and pairs that text with the metadata needed to paste deterministically
/// (document-space bbox at copy time, source element ids, and whether the copy
/// came from a whole-group selection).
///
/// On the system clipboard the payload is stored as plain text, prefixed with a
/// one-line header (`# donner-shape-clipboard v1`) followed by a small key/value
/// metadata block and the SVG fragment. Generic SVG text without the header is
/// still accepted by `parse()` as a best-effort import (the metadata fields are
/// left empty), which keeps interoperability with hand-authored fragments.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/Box.h"

namespace donner::editor {

/// Structured shape-clipboard payload. See file comment for the wire format.
struct ShapeClipboardPayload {
  /// Header line that identifies a Donner shape-clipboard payload.
  static constexpr std::string_view kHeader = "# donner-shape-clipboard v1";

  /// SVG fragment text: the serialized subtrees of the copied elements, in
  /// document paint order. Each top-level fragment node is one copied element.
  std::string svgFragment;

  /// Combined document-space bounding box of the copied elements at copy time.
  /// `std::nullopt` when no bounds were available (e.g. best-effort import).
  std::optional<Box2d> documentBounds;

  /// The `id` attribute of each copied top-level element, in the same order as
  /// they appear in `svgFragment`. Empty entries denote elements without an id.
  std::vector<std::string> sourceElementIds;

  /// True when the copy originated from a selection of one or more whole groups
  /// (`<g>`/`<svg>` containers). Used by paste to decide group-aware insertion.
  bool wasGroupSelection = false;

  /// Serialize this payload to the headered text form written to the clipboard.
  [[nodiscard]] std::string toClipboardText() const;

  /// Parse clipboard text into a payload.
  ///
  /// When the text begins with `kHeader`, the metadata block is parsed and the
  /// remaining bytes become `svgFragment`. Otherwise the entire text is treated
  /// as a best-effort SVG fragment with empty metadata. Returns `std::nullopt`
  /// only when no usable SVG fragment can be recovered (empty/whitespace text).
  [[nodiscard]] static std::optional<ShapeClipboardPayload> parse(std::string_view clipboardText);
};

}  // namespace donner::editor
