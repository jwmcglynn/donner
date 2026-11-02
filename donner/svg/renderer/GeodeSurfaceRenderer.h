#pragma once
/// @file
/// Presents SVG documents to a host-owned native surface with the Geode renderer.

#include <memory>
#include <string>

#include "donner/base/Vector2.h"
#include "donner/svg/SVGDocument.h"

namespace donner::svg {

/**
 * Renders SVG documents with Geode directly into a window surface the host owns, keeping every
 * frame on the GPU from rendering through presentation.
 *
 * This is the application-embedding entry point: it selects a GPU root that can present, creates
 * the Geode device and runtime surface, and acquires, draws and presents one frame per \ref draw.
 * The host keeps its platform object (on macOS, a `CAMetalLayer` attached to its view) alive for
 * the renderer's lifetime and destroys the renderer before it.
 *
 * Typical use from a macOS view:
 *
 * ```cpp
 * CAMetalLayer* layer = [CAMetalLayer layer];
 * view.layer = layer;
 * view.wantsLayer = YES;
 * std::string error;
 * auto presenter = GeodeSurfaceRenderer::CreateForMetalLayer((__bridge void*)layer,
 *                                                           Vector2i(width, height), &error);
 * // Each frame:
 * document.setCanvasSize(presenter->size().x, presenter->size().y);
 * presenter->draw(document);
 * ```
 *
 * Calls must come from one thread at a time. Only Metal layers are supported; Vulkan hosts use
 * the GPU runtime surface API directly (see the GPU runtime guide's Surfaces section).
 */
class GeodeSurfaceRenderer {
public:
  /// Outcome of one \ref draw call.
  enum class FrameStatus {
    Presented,  //!< The frame was drawn and handed to the surface for presentation.
    Skipped,    //!< No frame was available (timeout, or the surface was reconfigured). Retry.
    Failed,     //!< The surface or device was lost. Recreate the renderer to continue.
  };

  /**
   * Creates a renderer that presents to a Core Animation Metal layer.
   *
   * @param metalLayer The host's `CAMetalLayer*`, bridged to `void*`. It must outlive the renderer.
   *   The renderer configures the layer's device, pixel format and drawable size.
   * @param sizePx Drawable size in pixels. Both dimensions must be positive.
   * @param error When non-null, receives a description of why creation failed.
   * @return The renderer, or nullptr when the layer is null, the size is empty, no presenting GPU
   *   root is available, or the surface cannot present Geode's format.
   */
  static std::unique_ptr<GeodeSurfaceRenderer> CreateForMetalLayer(void* metalLayer,
                                                                   Vector2i sizePx,
                                                                   std::string* error = nullptr);

  /// Releases the surface, then the device. Waits, bounded, for submitted frames to finish.
  ~GeodeSurfaceRenderer();

  GeodeSurfaceRenderer(const GeodeSurfaceRenderer&) = delete;
  GeodeSurfaceRenderer& operator=(const GeodeSurfaceRenderer&) = delete;
  GeodeSurfaceRenderer(GeodeSurfaceRenderer&&) = delete;
  GeodeSurfaceRenderer& operator=(GeodeSurfaceRenderer&&) = delete;

  /**
   * Changes the drawable size, for example after the host view resizes or moves to a display with
   * a different scale.
   *
   * @param sizePx New size in pixels. Both dimensions must be positive.
   * @return False when the size is empty or the surface cannot be reconfigured; the previous size
   *   remains in effect.
   */
  [[nodiscard]] bool resize(Vector2i sizePx);

  /// Current drawable size in pixels.
  Vector2i size() const;

  /**
   * Draws \p document into the next surface frame and presents it.
   *
   * The document is drawn at its canvas size; set it to \ref size() to fill the surface.
   *
   * @param document Document to draw.
   * @return Whether a frame was presented.
   */
  [[nodiscard]] FrameStatus draw(SVGDocument& document);

  /// True once the GPU device has been lost; every later \ref draw returns Failed.
  bool isDeviceLost() const;

private:
  struct Impl;
  explicit GeodeSurfaceRenderer(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace donner::svg
