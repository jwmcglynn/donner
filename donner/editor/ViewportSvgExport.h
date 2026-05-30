#pragma once
/// @file
///
/// Viewport SVG export (Milestone 6 of `docs/design_docs/0047-v0_8_showcase.md`).
///
/// Exports the document region currently visible in the editor render pane as a
/// standalone, cropped SVG. The export is **vector-first**: the source SVG
/// children are copied verbatim into a clipped `<g>`, never snapshotted as a
/// raster `<image>`. The crop is derived entirely from \ref ViewportState — it
/// is the single source of truth for the screen↔document mapping.
///
/// This milestone covers *content-only* export. The optional editor selection
/// overlay is emitted only as an empty placeholder group; the overlay
/// primitives themselves are produced by Milestone 7's `OverlayRenderer` SVG
/// emit path. See \ref ViewportExportOptions::includeSelectionOverlay.

#include <string>

#include "donner/base/Box.h"
#include "donner/editor/ViewportState.h"
#include "donner/svg/SVGDocument.h"

namespace donner::editor {

/// Integer screen-pixel rectangle (top-left / bottom-right corners). Used for
/// the render pane content rect supplied to \ref ExportViewportAsSvg.
using Recti = Box2<int>;

/// A success-or-error value used by the viewport SVG exporter.
///
/// Either holds an exported value of type \p T, or a human-readable error
/// string of type \p E. Donner's `ParseResult` is purpose-built for parser
/// `ParseError`s; the exporter wants a plain string error message, so this
/// lightweight result type is used instead.
template <typename T, typename E>
struct Result {
  bool isOk = false;  ///< True when \ref value holds an exported result.
  T value{};          ///< Exported value. Valid only when \ref isOk is true.
  E error{};          ///< Error message. Valid only when \ref isOk is false.

  /// Construct a success result.
  static Result Ok(T value) { return Result{true, std::move(value), E{}}; }
  /// Construct an error result.
  static Result Err(E error) { return Result{false, T{}, std::move(error)}; }

  /// Whether this result holds a value.
  [[nodiscard]] bool ok() const { return isOk; }
};

/// Options controlling viewport SVG export behavior.
struct ViewportExportOptions {
  /// When true, the exported SVG preserves transparency (no background rect).
  /// When false, a covering background rect is prepended (white fallback).
  bool transparentBackground = true;
  /// When true, an `id="donner-editor-overlay"` group placeholder is emitted.
  /// In Milestone 6 the group is always empty; Milestone 7 populates it with
  /// serialized overlay primitives.
  bool includeSelectionOverlay = false;
};

/**
 * Export the currently-visible document region as a cropped, standalone SVG.
 *
 * The exported root `viewBox` is `viewport.screenToDocument(renderPaneRect)`
 * formatted as `min-x min-y width height`; `width`/`height` are the render pane
 * dimensions in CSS pixels. Source root attributes (`xmlns`, etc.) are carried
 * onto the exported root with `viewBox`/`width`/`height` replaced. The source
 * SVG children are wrapped verbatim in a `<g>` clipped to the document-space
 * viewport rect.
 *
 * The export reads from \p doc only; it does not reparse the active document
 * or clear any compositor cache. The source document is never mutated.
 *
 * Self-contained documents export successfully. Documents that reference
 * external resources over `http://`, `https://`, or `file://` via
 * `href` / `xlink:href` are refused with a human-readable error, since the
 * export cannot embed or safely reference them (vector-first; no rasterizing).
 *
 * @param doc Source SVG document. Must have an owned XML source store.
 * @param viewport Viewport state describing the screen↔document mapping.
 * @param renderPaneRect Render pane content rect in screen (CSS) pixels.
 * @param options Export options (background + overlay handling).
 * @return Exported SVG text, or a human-readable error string.
 */
Result<std::string, std::string> ExportViewportAsSvg(const svg::SVGDocument& doc,
                                                     const ViewportState& viewport,
                                                     const Recti& renderPaneRect,
                                                     const ViewportExportOptions& options);

}  // namespace donner::editor
