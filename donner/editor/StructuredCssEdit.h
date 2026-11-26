#pragma once
/// @file

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "donner/editor/FlashDecorations.h"

namespace donner::editor {

class DocumentSyncController;
class EditorApp;
class TextEditor;

/// XML source representation of the authored CSS item.
enum class StructuredCssSourceEncoding : std::uint8_t { Text, CData, Attribute, Unmapped };

/// Exact selection and edit boundaries for a source-backed CSS rule or declaration.
struct StructuredCssTarget {
  SourceByteRange rule;                        ///< Complete qualified rule.
  SourceByteRange selector;                    ///< Authored selector list.
  std::optional<SourceByteRange> declaration;  ///< Complete declaration and optional semicolon.
  std::optional<SourceByteRange> name;         ///< Authored property name.
  std::optional<SourceByteRange> value;        ///< Authored property value.
  std::optional<SourceByteRange> priority;     ///< Authored importance annotation.
  std::size_t insertDeclaration = 0;  ///< Position directly before the rule's closing brace.
  StructuredCssSourceEncoding encoding = StructuredCssSourceEncoding::Unmapped;  ///< Context.
  std::string decodedMoveText;  ///< Original CSS token bytes before XML encoding.
  char attributeQuote = '\0';   ///< Original delimiter for an inline style attribute.
  bool inlineStyle = false;     ///< The selected declaration belongs to a style attribute.
  bool declarationIncludesSemicolon = false;  ///< Selected bytes include a CSS separator.
  bool declarationNeedsSeparator = false;     ///< Last declaration has no terminating semicolon.
  std::size_t selectionOffset = 0;  ///< Original hit that resolves the same parsed target.
  std::uint64_t generation = 0;     ///< Document generation at selection.
  std::uint64_t sourceVersion = 0;  ///< Document source revision at selection.

  /// Compare resolved ranges and revision identity.
  bool operator==(const StructuredCssTarget&) const = default;
};

/// Structured edit to a CSS rule or declaration.
enum class StructuredCssOperation {
  ReplaceSelector,
  ReplaceName,
  ReplaceValue,
  AddImportant,
  RemoveImportant,
  InsertDeclaration,
  InsertRule,
  RemoveDeclaration,
  RemoveRule,
};

/// Locate parsed CSS covering a source offset in the current editor snapshot.
/// @param app Active editor document.
/// @param source Current source pane contents.
/// @param offset Byte position that identifies the target.
std::optional<StructuredCssTarget> FindStructuredCssTarget(EditorApp& app, std::string_view source,
                                                           std::size_t offset);

/// Apply one CSS operation as one document command and one document undo step.
/// @param app Active editor document.
/// @param sync Source owner that mirrors the committed document delta.
/// @param editor Source editor receiving the edit.
/// @param target Previously resolved, revision-bound CSS target.
/// @param operation Requested typed operation.
/// @param replacement Authored CSS replacement for replace or insert operations.
/// @return True if the operation changed the source buffer.
bool ApplyStructuredCssEdit(EditorApp& app, DocumentSyncController& sync, TextEditor& editor,
                            const StructuredCssTarget& target, StructuredCssOperation operation,
                            std::string_view replacement = {});

/// Check whether two revision-bound CSS targets can be reordered.
/// @param app Active editor document.
/// @param source Current source pane contents.
/// @param from Dragged CSS item.
/// @param destination Compatible destination of the same item kind.
/// @param after Insert after rather than before the destination.
bool CanMoveStructuredCss(EditorApp& app, std::string_view source, const StructuredCssTarget& from,
                          const StructuredCssTarget& destination, bool after);

/// Move a CSS item relative to another compatible item.
/// @param app Active editor document.
/// @param sync Source owner that mirrors the committed document delta.
/// @param editor Source editor receiving the committed source delta.
/// @param from Dragged CSS item.
/// @param destination Compatible destination of the same item kind.
/// @param after Insert after rather than before the destination.
/// @return True when the source changed.
bool MoveStructuredCss(EditorApp& app, DocumentSyncController& sync, TextEditor& editor,
                       const StructuredCssTarget& from, const StructuredCssTarget& destination,
                       bool after);

}  // namespace donner::editor
