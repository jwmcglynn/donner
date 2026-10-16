#pragma once
/// @file

#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace donner::svg {

/// One frame of a decoded GIF, composited onto the full logical screen.
struct DecodedGifFrame {
  /// Canvas after drawing this frame, straight RGBA8, `width * height * 4` bytes. Transparent
  /// pixels are (0, 0, 0, 0).
  std::vector<uint8_t> rgba;
  int left = 0;               //!< Image descriptor left edge.
  int top = 0;                //!< Image descriptor top edge.
  int width = 0;              //!< Image descriptor width.
  int height = 0;             //!< Image descriptor height.
  int disposal = 0;           //!< Disposal method from the graphic control extension.
  int delayCentiseconds = 0;  //!< Frame delay from the graphic control extension.
  int colorTableSize = 0;     //!< Entries in the color table used by this frame.
  /// Transparent color index from the graphic control extension, if transparency is enabled.
  std::optional<int> transparentIndex;
};

/// A decoded GIF file.
struct DecodedGif {
  int width = 0;                        //!< Logical screen width.
  int height = 0;                       //!< Logical screen height.
  std::optional<int> loopCount;         //!< NETSCAPE2.0 repeat count, if present.
  std::vector<DecodedGifFrame> frames;  //!< Frames in display order.
};

/**
 * Minimal GIF89a decoder for tests. Supports global and local color tables, the graphic control
 * and NETSCAPE2.0 extensions, transparency, and disposal methods 0-2. Interlaced images and
 * disposal 3 are rejected.
 *
 * @param data Encoded GIF bytes.
 * @param error Set to a description of the first problem when decoding fails.
 * @return The decoded GIF, or std::nullopt on malformed or unsupported input.
 */
std::optional<DecodedGif> DecodeGifForTesting(std::span<const uint8_t> data, std::string* error);

}  // namespace donner::svg
