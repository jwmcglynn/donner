#pragma once

#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace tiny_skia::examples {

/// Writes RGBA8 pixel data as an uncompressed-filter PNG file.
/// Expects straight (non-premultiplied) alpha, 4 bytes per pixel.
/// Returns true on success.
inline bool writePng(const std::string& path, const std::uint8_t* rgba, std::uint32_t width,
                     std::uint32_t height) {
  // Build raw scanline data: each row is [filter_byte=0] + [RGBA pixels].
  const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
  const std::size_t rawSize = static_cast<std::size_t>(height) * (1 + rowBytes);
  std::vector<std::uint8_t> raw(rawSize);

  for (std::uint32_t y = 0; y < height; ++y) {
    auto* dst = raw.data() + static_cast<std::size_t>(y) * (1 + rowBytes);
    dst[0] = 0;  // Filter: None
    std::memcpy(dst + 1, rgba + static_cast<std::size_t>(y) * rowBytes, rowBytes);
  }

  // Compress with zlib.
  uLongf compBound = compressBound(static_cast<uLong>(rawSize));
  std::vector<std::uint8_t> compressed(compBound);
  if (compress2(compressed.data(), &compBound, raw.data(), static_cast<uLong>(rawSize),
                Z_DEFAULT_COMPRESSION) != Z_OK) {
    return false;
  }
  compressed.resize(compBound);

  // Helper: write a big-endian 32-bit value.
  auto writeU32BE = [](std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
  };

  // Helper: write a PNG chunk (length + type + data + CRC).
  auto writeChunk = [&](std::ofstream& out, const char* type, const std::uint8_t* data,
                        std::uint32_t len) {
    std::uint8_t header[8];
    writeU32BE(header, len);
    std::memcpy(header + 4, type, 4);
    out.write(reinterpret_cast<const char*>(header), 8);

    // CRC covers type + data.
    auto crc = crc32(0L, reinterpret_cast<const Bytef*>(type), 4);
    if (len > 0) {
      out.write(reinterpret_cast<const char*>(data), len);
      crc = crc32(crc, data, len);
    }
    std::uint8_t crcBuf[4];
    writeU32BE(crcBuf, static_cast<std::uint32_t>(crc));
    out.write(reinterpret_cast<const char*>(crcBuf), 4);
  };

  std::ofstream file(path, std::ios::binary);
  if (!file) return false;

  // PNG signature.
  constexpr std::uint8_t sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  file.write(reinterpret_cast<const char*>(sig), 8);

  // IHDR: width, height, bit_depth=8, color_type=6 (RGBA), compression=0,
  //       filter=0, interlace=0.
  std::uint8_t ihdr[13];
  writeU32BE(ihdr, width);
  writeU32BE(ihdr + 4, height);
  ihdr[8] = 8;   // bit depth
  ihdr[9] = 6;   // color type: RGBA
  ihdr[10] = 0;  // compression
  ihdr[11] = 0;  // filter
  ihdr[12] = 0;  // interlace
  writeChunk(file, "IHDR", ihdr, 13);

  // IDAT
  writeChunk(file, "IDAT", compressed.data(), static_cast<std::uint32_t>(compressed.size()));

  // IEND
  writeChunk(file, "IEND", nullptr, 0);

  return file.good();
}

}  // namespace tiny_skia::examples
