#pragma once
/// @file

#include <memory>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg {

/**
 * Backend-agnostic renderer that resolves to the active build backend.
 *
 * Clients should prefer this type when they do not need backend-specific APIs.
 */
class Renderer {
public:
  /**
   * Creates a renderer for the active backend.
   *
   * @param verbose If true, enables backend-specific verbose logging.
   */
  explicit Renderer(bool verbose = false);

  /// Destructor.
  ~Renderer();

  /// Move constructor.
  Renderer(Renderer&&) noexcept;

  /// Move assignment operator.
  Renderer& operator=(Renderer&&) noexcept;

  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  /**
   * Draws the SVG document using the active backend.
   *
   * @param document The SVG document to render.
   */
  void draw(SVGDocument& document);

  /**
   * Captures a CPU-readable snapshot of the current frame.
   *
   * @return A snapshot of the rendered frame.
   */
  [[nodiscard]] RendererBitmap takeSnapshot() const;

  /**
   * Saves the last rendered frame to a PNG file.
   *
   * @param filename The output PNG filename.
   * @return True if the file was written.
   */
  bool save(const char* filename);

  /**
   * Returns the rendered width in pixels.
   *
   * @return The rendered width.
   */
  [[nodiscard]] int width() const;

  /**
   * Returns the rendered height in pixels.
   *
   * @return The rendered height.
   */
  [[nodiscard]] int height() const;

private:
  std::unique_ptr<RendererInterface> impl_;
};

}  // namespace donner::svg
