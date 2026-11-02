#pragma once
/// @file
/// Presents SVG documents to a host-owned window surface (a Core Animation Metal layer or a browser
/// canvas) with the Geode renderer.

#include <memory>
#include <ostream>
#include <string>
#include <string_view>

#include "donner/base/Vector2.h"

namespace donner::svg {

class SVGDocument;

/**
 * Renders SVG documents with Geode directly into a window surface the host owns, keeping every
 * frame on the GPU from rendering through presentation. Available with Geode enabled
 * (`donner.configure(renderer = "geode")`) on macOS, presenting to a `CAMetalLayer`
 * (\ref CreateForMetalLayer), and in WebAssembly builds with the browser backend
 * (`--@donner//donner/svg/renderer/geode:browser_backend=true`), presenting to a `<canvas>`
 * (\ref CreateForCanvas).
 *
 * This is the application-embedding entry point: it opens a GPU root, the Geode device and a
 * runtime surface, and acquires, draws and presents one frame per \ref draw.
 *
 * Metal layer contract:
 * - The layer outlives the renderer. Destroy the renderer first.
 * - The drawable size passed to \ref CreateForMetalLayer and \ref resize is in pixels and must
 *   equal the layer's `bounds.size` multiplied by its `contentsScale`, rounded to the nearest
 *   pixel on each axis. Keep `contentsScale` equal to the display's backing scale; a
 *   layer-hosting view does not update it for you. When the
 *   layer's pixel extent no longer matches, \ref draw returns \ref FrameStatus::ResizeNeeded and
 *   presents nothing until \ref resize is called with the new size. A layer with empty bounds
 *   (not laid out in a window) is never reported as resized.
 * - The renderer configures the layer's `device`, `pixelFormat` (BGRA8), `drawableSize` and
 *   `opaque` properties. Do not change them yourself.
 * - The layer is configured opaque, so transparent regions of a document composite as black.
 *   Give the document a background to control the color.
 * - A frame target must fit the renderer's surface budget (4 bytes per pixel within
 *   `RendererSurfaceBudget::kMaximumBytes`); larger sizes are rejected. Documents whose layers,
 *   masks or patterns need more storage near that limit can still fail to draw.
 * - Calls must come from one thread at a time.
 *
 * Typical use from a macOS view that is already in a window:
 *
 * ```cpp
 * CAMetalLayer* layer = [CAMetalLayer layer];
 * view.layer = layer;
 * view.wantsLayer = YES;
 * layer.contentsScale = view.window.backingScaleFactor;
 * const NSSize px = [view convertSizeToBacking:view.bounds.size];
 * const Vector2i sizePx(static_cast<int>(std::lround(px.width)),
 *                       static_cast<int>(std::lround(px.height)));
 * std::string error;
 * auto presenter =
 *     GeodeSurfaceRenderer::CreateForMetalLayer((__bridge void*)layer, sizePx, &error);
 * // Each frame, after updating contentsScale and calling resize() if the view changed:
 * document.setCanvasSize(presenter->size().x, presenter->size().y);
 * presenter->draw(document);
 * ```
 *
 * Canvas contract:
 * - Create the renderer from a context that can wait for the browser's asynchronous WebGPU device
 *   request, for example `main()` in a module linked with `-sASYNCIFY`, then draw from the
 *   per-frame loop.
 * - Pass the canvas drawing-buffer size in pixels (its CSS size times `devicePixelRatio`) and call
 *   \ref resize when it changes. The browser presents each drawn frame at the end of the current
 *   task.
 * - The canvas is configured with the browser's preferred format (BGRA8 or RGBA8) and opaque
 *   alpha.
 *
 * Linux (Vulkan) window presentation has no public entry point yet.
 */
class GeodeSurfaceRenderer {
public:
  /// Outcome of one \ref draw call.
  enum class FrameStatus {
    /// The frame was drawn and handed to the layer for presentation.
    Presented,
    /// No drawable was available in time. Nothing was drawn; draw again next frame.
    Skipped,
    /// The surface's pixel extent differs from the configured size. Nothing was drawn; call
    /// \ref resize with the current size (for a Metal layer, `bounds.size * contentsScale`), then
    /// draw again.
    ResizeNeeded,
    /// The device was lost, or the surface refused a frame. Recreate the renderer to continue.
    Failed,
  };

  /**
   * Creates a renderer that presents to a Core Animation Metal layer.
   *
   * @param metalLayer The host's `CAMetalLayer*`, bridged to `void*`. It must outlive the renderer.
   * @param sizePx Drawable size in pixels (`bounds.size * contentsScale`). Both dimensions must be
   *   positive.
   * @param error When non-null, receives a description of why creation failed.
   * @return The renderer, or nullptr when the layer is null or not a `CAMetalLayer`, the size is
   *   empty or exceeds the surface budget, Metal is unavailable, or the surface cannot present
   *   BGRA8 render targets.
   */
  static std::unique_ptr<GeodeSurfaceRenderer> CreateForMetalLayer(void* metalLayer,
                                                                   Vector2i sizePx,
                                                                   std::string* error = nullptr);

  /**
   * Creates a renderer that presents to a browser `<canvas>` element through WebGPU.
   *
   * @param canvasSelector CSS selector naming the canvas, such as `"#canvas"`. In a worker, which
   *   has no document, only a plain id selector resolves.
   * @param sizePx Drawing-buffer size in pixels. Both dimensions must be positive.
   * @param error When non-null, receives a description of why creation failed.
   * @return The renderer, or nullptr when the selector is empty, the size is empty, the build has
   *   no browser backend, the browser offers no WebGPU device, or the canvas cannot present.
   */
  static std::unique_ptr<GeodeSurfaceRenderer> CreateForCanvas(std::string_view canvasSelector,
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
   * a different scale. Resizing to the current size does nothing.
   *
   * @param sizePx New size in pixels (for a Metal layer, `bounds.size * contentsScale`). Both
   *   dimensions must be positive.
   * @param error When non-null, receives a description of why the resize failed.
   * @return False when the size is empty, exceeds the surface budget, or the surface cannot be
   *   reconfigured; the previous size remains in effect.
   */
  [[nodiscard]] bool resize(Vector2i sizePx, std::string* error = nullptr);

  /// Current drawable size in pixels.
  Vector2i size() const;

  /**
   * Draws \p document into the next surface frame and presents it.
   *
   * The document is drawn at its canvas size; set it to \ref size() to fill the surface.
   *
   * @param document Document to draw.
   * @return Whether a frame was presented, and what the host should do otherwise.
   */
  [[nodiscard]] FrameStatus draw(SVGDocument& document);

  /// True once the GPU device has been lost; every later \ref draw returns Failed.
  bool isDeviceLost() const;

private:
  struct Impl;
  struct SurfaceRequest;
  explicit GeodeSurfaceRenderer(std::unique_ptr<Impl> impl);

  /// Opens a renderer for the platform object \p request names.
  static std::unique_ptr<GeodeSurfaceRenderer> Create(const SurfaceRequest& request,
                                                      Vector2i sizePx, std::string* error);

  std::unique_ptr<Impl> impl_;
};

/// Writes the enumerator name of \p status, for logs and test failure messages.
std::ostream& operator<<(std::ostream& os, GeodeSurfaceRenderer::FrameStatus status);

}  // namespace donner::svg
