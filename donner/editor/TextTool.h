#pragma once
/// @file
///
/// `TextTool` is the editor's text-authoring tool. A single canvas click
/// creates a source-backed `<text>` element at the clicked document-space
/// point with default content and style, after which the shell switches back
/// to the Select tool so the new text can be moved, resized, and edited in the
/// inspector. There is no in-canvas caret in v0.8 (see
/// `docs/design_docs/0047-v0_8_showcase.md` § "Text Authoring").

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/Vector2.h"
#include "donner/editor/Tool.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

/// Click-to-place `<text>` authoring tool.
///
/// The tool is single-shot: one `onMouseDown` inserts a `<text>` element and
/// records the inserted element plus the prior selection so the insertion can
/// be undone and redone through the same mutation seam.
class TextTool final : public Tool {
public:
  /// Default text content placed in a newly-created `<text>` element.
  static constexpr std::string_view kDefaultContent = "Text";
  /// Default `font-family` attribute applied to new text.
  static constexpr std::string_view kDefaultFontFamily = "sans-serif";
  /// Default `font-size` attribute applied to new text.
  static constexpr std::string_view kDefaultFontSize = "32";
  /// Default `fill` attribute applied to new text.
  static constexpr std::string_view kDefaultFill = "black";

  /// Handle a click in document space: create a `<text>` element at
  /// `documentPoint` inside the selected compatible parent (a `<g>` or the
  /// root `<svg>`) or, when no compatible parent is selected, the root
  /// `<svg>`. The new text is selected; the previous selection is recorded
  /// for undo.
  ///
  /// @param editor Editor state and mutation queue.
  /// @param documentPoint Click location in SVG document coordinates.
  /// @param modifiers Modifier-key state for the click (unused).
  void onMouseDown(EditorApp& editor, const Vector2d& documentPoint,
                   MouseModifiers modifiers) override;

  /// No-op: the text tool acts on a single click only.
  void onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld) override;

  /// No-op: the text tool acts on a single click only.
  void onMouseUp(EditorApp& editor, const Vector2d& documentPoint) override;

  /// Discard pending tool state without modifying the current document. Does
  /// not undo an already-applied insertion.
  void cancel();

  /// The most recently inserted `<text>` element, or `std::nullopt` if the
  /// tool has not inserted anything since the last `cancel()`.
  [[nodiscard]] const std::optional<svg::SVGElement>& insertedTextElement() const {
    return insertedText_;
  }

  /// Undo the most recent insertion by removing the inserted element and
  /// restoring the previously-selected elements. Routes through
  /// `DeleteElementCommand` so the removal goes through the mutation seam.
  /// No-op if nothing has been inserted.
  void undoInsert(EditorApp& editor);

  /// Redo a previously-undone insertion by re-inserting the same element with
  /// the same content and selecting it. Routes through `InsertTextCommand`.
  /// No-op unless an insertion was made.
  void redoInsert(EditorApp& editor);

private:
  /// Inserted `<text>` element (valid after `onMouseDown` until `cancel`).
  std::optional<svg::SVGElement> insertedText_;
  /// Parent the text was inserted into; used to re-insert on redo.
  std::optional<svg::SVGElement> insertedParent_;
  /// Selection in effect immediately before insertion; restored on undo.
  std::vector<svg::SVGElement> previousSelection_;
  /// Text content placed in the element; reused on redo.
  std::string content_;
};

}  // namespace donner::editor
