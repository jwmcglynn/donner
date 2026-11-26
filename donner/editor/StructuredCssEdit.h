#pragma once
/// @file

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "donner/editor/FlashDecorations.h"

namespace donner::editor {

class EditorApp;
class TextEditor;

/// Exact selection and edit boundaries for a source-backed CSS rule or declaration.
struct StructuredCssTarget {
  SourceByteRange rule;                        ///< Complete qualified rule.
  SourceByteRange selector;                    ///< Authored selector list.
  std::optional<SourceByteRange> declaration;  ///< Complete declaration and optional semicolon.
  std::optional<SourceByteRange> name;         ///< Authored property name.
  std::optional<SourceByteRange> value;        ///< Authored property value.
  std::optional<SourceByteRange> priority;     ///< Authored importance annotation.
  std::size_t insertDeclaration = 0;       ///< Position directly before the rule's closing brace.
  bool declarationNeedsSeparator = false;  ///< Last declaration has no terminating semicolon.
  std::size_t selectionOffset = 0;         ///< Original hit that resolves the same parsed target.
  std::uint64_t generation = 0;            ///< Document generation at selection.
  std::uint64_t sourceVersion = 0;         ///< Document source revision at selection.

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

/// Apply one CSS operation as one ordinary source-edit intent and undo step.
/// @param app Active editor document.
/// @param editor Source editor receiving the edit.
/// @param target Previously resolved, revision-bound CSS target.
/// @param operation Requested typed operation.
/// @param replacement Authored CSS replacement for replace or insert operations.
/// @return True if the operation changed the source buffer.
bool ApplyStructuredCssEdit(EditorApp& app, TextEditor& editor, const StructuredCssTarget& target,
                            StructuredCssOperation operation, std::string_view replacement = {});

/// Move a rule before another rule, or a declaration before a declaration in the same rule.
/// @param app Active editor document.
/// @param editor Source editor receiving the move as one undoable intent.
/// @param from Revision-bound source target.
/// @param before Revision-bound destination target.
/// @return True if the targets share a CSS container and the source changed.
bool MoveStructuredCssBefore(EditorApp& app, TextEditor& editor, const StructuredCssTarget& from,
                             const StructuredCssTarget& before);

}  // namespace donner::editor
