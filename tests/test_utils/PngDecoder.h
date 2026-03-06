#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tiny_skia::test_utils {

/// Result of decoding a PNG file.
struct DecodedPng {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  /// Raw RGBA8 pixel data (straight alpha, 4 bytes per pixel).
  std::vector<std::uint8_t> data;
};

/// Decodes a PNG file into raw RGBA8 pixel data.
/// Supports 8-bit RGB and RGBA PNGs (no interlacing).
/// Returns nullopt on failure.
[[nodiscard]] std::optional<DecodedPng> decodePng(const std::string& path);

}  // namespace tiny_skia::test_utils
