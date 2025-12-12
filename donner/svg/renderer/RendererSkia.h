#pragma once
/// @file

#include <string>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/SVGDocument.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkGraphics.h"

namespace donner::svg {

/**
 * Rendering backend using Skia, https://github.com/google/skia
 *
 * Skia is a 2D graphics library that powers Chrome, Firefox, Android, and many other projects, and
 * supports all functionality required to implement SVG (as many of these projects also support
 * SVG).
 *
 * Skia is used as the reference renderer while implementing Donner, but long-term Donner would like
 * to support other rendering backends, so dependencies on Skia should be kept to a minimum and
 * isolated to RendererSkia.
 *
 * This is a prototype-quality implementation, and is subject to refactoring in the future to
 * provide a cleaner API boundary between Donner and the rendering backend.
 */
class RendererSkia {
public:
  /**
   * Create the Skia renderer.
   *
   * @param verbose  If true, print verbose logging.
   */
  explicit RendererSkia(bool verbose = false);

  /// Destructor.
  ~RendererSkia();

  // Move-only, no copy.
  /// Move constructor.
  RendererSkia(RendererSkia&&) noexcept;
  /// Move assignment operator.
  RendererSkia& operator=(RendererSkia&&) noexcept;
  RendererSkia(const RendererSkia&) = delete;
  RendererSkia& operator=(const RendererSkia&) = delete;

  /**
   * Draw the SVG document using the renderer. Writes to an internal bitmap, which can be
   * retrieved using the bitmap() method.
   *
   * @param document The SVG document to render.
   */
  void draw(SVGDocument& document);

  /**
   * Render the given \ref SVGDocument into ASCII art. The generated image is of given size, and has
   * a black background.
   *
   * Colors will be mapped to ASCII characters, with `@` white all the way to `.` black, with ten
   * shades of gray.
   *
   * For example:
   * ```
   * <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
   *   <rect width="8" height="8" fill="white" />
   * </svg>
   * ```
   *
   * Renders as:
   * ```
   * @@@@@@@@........
   * @@@@@@@@........
   * @@@@@@@@........
   * @@@@@@@@........
   * @@@@@@@@........
   * @@@@@@@@........
   * @@@@@@@@........
   * @@@@@@@@........
   * ................
   * ................
   * ................
   * ................
   * ................
   * ................
   * ................
   * ................
   * ```
   *
   * @param document SVG document to render, of max size 64x64.
   */
  std::string drawIntoAscii(SVGDocument& document);

  /**
   * Draw the given \ref SVGDocument into a SkPicture, for offscreen rendering or debugging
   * purposes.
   *
   * @param document The SVG document to render.
   */
  sk_sp<SkPicture> drawIntoSkPicture(SVGDocument& document);

  /**
   * Save the rendered image to a PNG file.
   *
   * @param filename The filename to save the PNG to.
   * @return True if the save was successful.
   */
  bool save(const char* filename);

  /**
   * Get the pixel data of the rendered image.
   *
   * @return A span of the pixel data, in RGBA format of size `width() * height() * 4`.
   */
  std::span<const uint8_t> pixelData() const;

  /// Get the width of the rendered image in pixels.
  int width() const { return bitmap_.width(); }

  /// Get the height of the rendered image in pixels.
  int height() const { return bitmap_.height(); }

  /// Get the SkBitmap of the rendered image.
  const SkBitmap& bitmap() const { return bitmap_; }

  /**
   * Enable or disable antialiasing. On by default.
   *
   * @param antialias Whether to enable antialiasing.
   */
  void setAntialias(bool antialias) { antialias_ = antialias; }

private:
  /// Implementation class.
  class Impl;

  /**
   * Internal helper to draw the given entity.
   *
   * @param registry Registry to use for drawing.
   */
  void draw(Registry& registry);

  bool verbose_;  //!< If true, print verbose logging.

  sk_sp<class SkFontMgr> fontMgr_;  //!< Font manager, may be initialized with custom fonts.
  sk_sp<class SkTypeface> fallbackTypeface_;  //!< Default fallback typeface for text.
  std::map<std::string, std::vector<sk_sp<SkTypeface>>> typefaces_;  //!< Cached typefaces by
                                                                     //!< family name.

  SkBitmap bitmap_;                    //!< The bitmap to render into.
  SkCanvas* rootCanvas_ = nullptr;     //!< The root canvas.
  SkCanvas* currentCanvas_ = nullptr;  //!< The current canvas.
  bool antialias_ = true;              //!< Whether to antialias.
};

}  // namespace donner::svg
