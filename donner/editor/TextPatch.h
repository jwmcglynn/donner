#pragma once
/// @file
///
/// The `TextPatch` type and `applyPatches` function form the editor's
/// canvas-to-text writeback sideband. When a tool mutates an attribute
/// via `EditorApp::applyMutation`, it also produces a `TextPatch` that
/// describes the corresponding byte-level splice in the source text.
/// The main loop drains pending patches after `flushFrame()` and before
/// rendering the source pane.
///
/// See `docs/design_docs/structured_text_editing.md` M3 for the full
/// design.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace donner::editor {

/**
 * A byte-level splice in the source text buffer.
 *
 * Represents "replace `length` bytes starting at `offset` with `replacement`."
 * Patches are applied in **descending offset order** by \ref applyPatches so
 * earlier patches don't shift the byte offsets of later ones.
 */
struct TextPatch {
  std::size_t offset = 0;       ///< Byte offset in the source text.
  std::size_t length = 0;       ///< Number of bytes to replace (0 = pure insert).
  std::string replacement;      ///< New text to splice in (empty = pure delete).
};

/**
 * Result of an \ref applyPatches call.
 */
struct ApplyPatchesResult {
  std::size_t applied = 0;           ///< Number of patches successfully applied.
  std::size_t rejectedBounds = 0;    ///< Patches whose offset+length exceeded the buffer.
};

/**
 * Apply a batch of \ref donner::editor::TextPatch "TextPatch" values to a source string.
 *
 * Patches are sorted by **descending offset** before application so each splice
 * doesn't shift the byte offsets of subsequent patches. Patches that would read
 * past the end of the current string are rejected (counted in
 * `result.rejectedBounds`). The remaining patches are applied in place.
 *
 * @param source The mutable source text. Modified in place.
 * @param patches The patches to apply. Order does not matter — the function
 *   sorts internally.
 * @return Summary of how many patches were applied vs. rejected.
 */
ApplyPatchesResult applyPatches(std::string& source, std::span<const TextPatch> patches);

}  // namespace donner::editor
