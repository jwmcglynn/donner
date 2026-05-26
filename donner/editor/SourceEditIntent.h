#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <string>

namespace donner::editor {

/// High-level source edit operation kind captured from the text editor.
enum class SourceEditIntentKind {
  Unknown,  ///< Operation kind was not classified by the caller.
  Insert,   ///< Text was inserted without removing existing bytes.
  Delete,   ///< Existing bytes were removed without inserting replacement text.
  Replace,  ///< Existing bytes were replaced with new text.
  Undo,     ///< The edit came from text-editor undo.
  Redo,     ///< The edit came from text-editor redo.
};

/// Line/column coordinate attached to a source edit boundary.
struct SourceEditPoint {
  int line = 0;    ///< Zero-based source line.
  int column = 0;  ///< Zero-based source column.

  bool operator==(const SourceEditPoint& other) const = default;
};

/// One source-buffer edit expressed in byte offsets against the buffer version
/// visible when the edit occurred.
struct SourceEditIntent {
  std::size_t offset = 0;         ///< Byte offset where the edit starts.
  std::size_t removedLength = 0;  ///< Number of bytes removed at \ref offset.
  std::string replacement;        ///< Bytes inserted at \ref offset.
  SourceEditIntentKind kind = SourceEditIntentKind::Unknown;
  std::uint64_t bufferVersion = 0;  ///< Monotonic text-buffer edit version.
  SourceEditPoint start;            ///< Edit start in the pre-edit source buffer.
  SourceEditPoint removedEnd;       ///< End of the removed range in the pre-edit buffer.
  SourceEditPoint replacementEnd;   ///< End of the inserted text in the post-edit buffer.

  bool operator==(const SourceEditIntent& other) const = default;
};

}  // namespace donner::editor
