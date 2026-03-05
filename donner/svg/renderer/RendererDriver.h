#pragma once
/// @file

#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/common/RenderingInstanceView.h"

namespace donner::svg {

/**
 * Backend-agnostic renderer driver that prepares documents for rendering and
 * emits drawing commands through a \ref RendererInterface implementation.
 */
class RendererDriver {
public:
  /**
   * Create a renderer driver that will forward traversal output to the given
   * backend implementation.
   */
  explicit RendererDriver(RendererInterface& renderer, bool verbose = false);

  /**
   * Render the given \ref SVGDocument using the configured backend.
   */
  void draw(SVGDocument& document);

  /**
   * Capture a snapshot from the underlying backend after rendering.
   */
  [[nodiscard]] RendererBitmap takeSnapshot() const;

private:
  void traverse(RenderingInstanceView& view, Registry& registry);

  RendererInterface& renderer_;
  bool verbose_ = false;
};

}  // namespace donner::svg
