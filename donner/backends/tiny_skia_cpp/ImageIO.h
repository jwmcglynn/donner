#pragma once
/// @file

#include <string>
#include <string_view>
#include <variant>

#include "donner/backends/tiny_skia_cpp/Expected.h"
#include "donner/backends/tiny_skia_cpp/Pixmap.h"

namespace donner::backends::tiny_skia_cpp {

/** Describes PNG read/write failures. */
struct PngError {
  std::string message;  //!< Human-readable error message.
};

/**
 * Tiny-skia PNG helpers that reuse stb_image/stb_image_write through the repository's existing
 * dependencies.
 */
class ImageIO {
public:
  /// Loads an RGBA pixmap from disk.
  static Expected<Pixmap, PngError> LoadRgbaPng(std::string_view filename);

  /// Writes an RGBA pixmap to disk.
  static Expected<std::monostate, PngError> WriteRgbaPng(std::string_view filename,
                                                         const Pixmap& pixmap);
};

}  // namespace donner::backends::tiny_skia_cpp
