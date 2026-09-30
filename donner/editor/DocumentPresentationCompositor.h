#pragma once
/// @file
/// Explicit intermediate-texture composition for non-WGPU document tiles.

#include <memory>
#include <optional>
#include <span>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/Vector2.h"
#include "donner/editor/DocumentCompositeTexture.h"
#include "donner/editor/FramePresentation.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/ViewportState.h"

namespace donner::editor {

/**
 * Composes cached non-WGPU document tiles into one transparent GL texture.
 *
 * The compositor preserves each tile's full affine presentation quad, including live drag and
 * transform previews. It performs the tile blend in premultiplied form, resolves back to straight
 * alpha for ImGui, and caches the result while all presentation inputs remain unchanged.
 */
class DocumentPresentationCompositor {
public:
  DocumentPresentationCompositor();
  ~DocumentPresentationCompositor();

  DocumentPresentationCompositor(const DocumentPresentationCompositor&) = delete;
  DocumentPresentationCompositor& operator=(const DocumentPresentationCompositor&) = delete;

  /**
   * Compose the current document presentation into one pane-sized texture.
   *
   * @param frame Validated camera, clip and resolved tile geometry shared with chrome.
   * @return Cached or newly composed texture view, or an empty view when composition is invalid.
   */
  [[nodiscard]] DocumentCompositeTextureView compose(const FramePresentation& frame);

  /// Invalidate the cached request and clear its presented view while retaining allocations.
  void reset();

  /// Bytes retained by the two RGBA8 intermediate textures.
  [[nodiscard]] std::uint64_t retainedBytes() const;

  /// Number of cache-miss compositions, exposed for deterministic performance tests.
  [[nodiscard]] std::uint64_t compositionCountForTesting() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace donner::editor
