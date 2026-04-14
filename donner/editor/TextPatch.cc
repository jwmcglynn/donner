#include "donner/editor/TextPatch.h"

#include <algorithm>
#include <numeric>

namespace donner::editor {

ApplyPatchesResult applyPatches(std::string& source, std::span<const TextPatch> patches) {
  ApplyPatchesResult result;

  if (patches.empty()) {
    return result;
  }

  // Build an index sorted by descending offset so each splice doesn't
  // invalidate the byte offsets of subsequent ones.
  std::vector<std::size_t> indices(patches.size());
  std::iota(indices.begin(), indices.end(), 0);
  std::sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
    return patches[a].offset > patches[b].offset;
  });

  for (const std::size_t idx : indices) {
    const TextPatch& patch = patches[idx];

    // Bounds check: offset must be within the string, and offset + length
    // must not exceed it. Use the overflow-safe form (no addition).
    if (patch.offset > source.size() || patch.length > source.size() - patch.offset) {
      ++result.rejectedBounds;
      continue;
    }

    source.replace(patch.offset, patch.length, patch.replacement);
    ++result.applied;
  }

  return result;
}

}  // namespace donner::editor
