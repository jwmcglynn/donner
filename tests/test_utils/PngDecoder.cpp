#include "PngDecoder.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include <zlib.h>

namespace tiny_skia::test_utils {

namespace {

// PNG signature bytes.
constexpr std::array<std::uint8_t, 8> kPngSignature = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

std::uint32_t readU32BE(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) |
         (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) |
         static_cast<std::uint32_t>(p[3]);
}

// Paeth predictor used in PNG scanline filtering.
std::uint8_t paethPredictor(std::uint8_t a, std::uint8_t b, std::uint8_t c) {
  int p = static_cast<int>(a) + static_cast<int>(b) - static_cast<int>(c);
  int pa = std::abs(p - static_cast<int>(a));
  int pb = std::abs(p - static_cast<int>(b));
  int pc = std::abs(p - static_cast<int>(c));
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

// Unfilter a single scanline in-place given the filter type and the previous
// scanline.  `bpp` is the number of bytes per pixel.
bool unfilterScanline(std::uint8_t* row, const std::uint8_t* prev,
                      std::uint32_t length, std::uint8_t filterType,
                      std::uint32_t bpp) {
  switch (filterType) {
    case 0:  // None
      break;
    case 1:  // Sub
      for (std::uint32_t i = bpp; i < length; ++i) {
        row[i] = static_cast<std::uint8_t>(row[i] + row[i - bpp]);
      }
      break;
    case 2:  // Up
      for (std::uint32_t i = 0; i < length; ++i) {
        row[i] = static_cast<std::uint8_t>(row[i] + prev[i]);
      }
      break;
    case 3:  // Average
      for (std::uint32_t i = 0; i < length; ++i) {
        std::uint8_t a = (i >= bpp) ? row[i - bpp] : 0;
        std::uint8_t b = prev[i];
        row[i] = static_cast<std::uint8_t>(
            row[i] + static_cast<std::uint8_t>((static_cast<int>(a) + static_cast<int>(b)) / 2));
      }
      break;
    case 4:  // Paeth
      for (std::uint32_t i = 0; i < length; ++i) {
        std::uint8_t a = (i >= bpp) ? row[i - bpp] : 0;
        std::uint8_t b = prev[i];
        std::uint8_t c = (i >= bpp) ? prev[i - bpp] : 0;
        row[i] =
            static_cast<std::uint8_t>(row[i] + paethPredictor(a, b, c));
      }
      break;
    default:
      return false;  // Unknown filter type.
  }
  return true;
}

}  // namespace

std::optional<DecodedPng> decodePng(const std::string& path) {
  // Read entire file.
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::nullopt;

  std::vector<std::uint8_t> fileData(
      (std::istreambuf_iterator<char>(file)),
      std::istreambuf_iterator<char>());

  if (fileData.size() < 8) return std::nullopt;

  // Verify PNG signature.
  if (!std::equal(kPngSignature.begin(), kPngSignature.end(),
                  fileData.begin())) {
    return std::nullopt;
  }

  // Parse chunks.
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint8_t bitDepth = 0;
  std::uint8_t colorType = 0;
  bool hasIhdr = false;
  std::vector<std::uint8_t> compressedData;
  std::vector<std::uint8_t> palette;   // PLTE chunk: R,G,B triples
  std::vector<std::uint8_t> trnsData;  // tRNS chunk: alpha per palette entry

  std::size_t pos = 8;
  while (pos + 12 <= fileData.size()) {
    std::uint32_t chunkLen = readU32BE(&fileData[pos]);
    const auto* chunkType =
        reinterpret_cast<const char*>(&fileData[pos + 4]);
    const auto* chunkData = &fileData[pos + 8];

    if (pos + 12 + chunkLen > fileData.size()) break;

    if (std::memcmp(chunkType, "IHDR", 4) == 0 && chunkLen >= 13) {
      width = readU32BE(chunkData);
      height = readU32BE(chunkData + 4);
      bitDepth = chunkData[8];
      colorType = chunkData[9];
      std::uint8_t compression = chunkData[10];
      std::uint8_t filter = chunkData[11];
      std::uint8_t interlace = chunkData[12];

      if (compression != 0 || filter != 0 || interlace != 0) {
        return std::nullopt;
      }
      // Support grayscale (0), RGB (2), indexed (3), grayscale+alpha (4),
      // RGBA (6). For non-indexed types require 8-bit depth; for indexed
      // allow 1, 2, 4, or 8.
      if (colorType == 3) {
        if (bitDepth != 1 && bitDepth != 2 && bitDepth != 4 &&
            bitDepth != 8) {
          return std::nullopt;
        }
      } else if (colorType == 0 || colorType == 2 || colorType == 4 ||
                 colorType == 6) {
        if (bitDepth != 8) {
          return std::nullopt;
        }
      } else {
        return std::nullopt;
      }
      hasIhdr = true;
    } else if (std::memcmp(chunkType, "PLTE", 4) == 0) {
      palette.assign(chunkData, chunkData + chunkLen);
    } else if (std::memcmp(chunkType, "tRNS", 4) == 0) {
      trnsData.assign(chunkData, chunkData + chunkLen);
    } else if (std::memcmp(chunkType, "IDAT", 4) == 0) {
      compressedData.insert(compressedData.end(), chunkData,
                            chunkData + chunkLen);
    } else if (std::memcmp(chunkType, "IEND", 4) == 0) {
      break;
    }

    pos += 12 + chunkLen;  // 4 len + 4 type + data + 4 crc
  }

  if (!hasIhdr || width == 0 || height == 0 || compressedData.empty()) {
    return std::nullopt;
  }
  if (colorType == 3 && palette.empty()) {
    return std::nullopt;
  }

  // Determine bytes per pixel for the raw (filtered) scanline data.
  // For sub-byte indexed images, pixels are packed into bytes; the stride
  // is ceil(width * bitDepth / 8).  For filtering, bpp = max(1, ...).
  std::uint32_t srcBpp;      // bytes per pixel (for filter; min 1)
  std::uint32_t srcStride;   // bytes per scanline (excluding filter byte)
  if (colorType == 3) {
    srcBpp = 1;  // sub-byte pixels: filter operates on bytes, bpp=1
    srcStride = (width * bitDepth + 7) / 8;
  } else {
    switch (colorType) {
      case 0:
        srcBpp = 1;
        break;  // Grayscale
      case 2:
        srcBpp = 3;
        break;  // RGB
      case 4:
        srcBpp = 2;
        break;  // Grayscale + Alpha
      case 6:
        srcBpp = 4;
        break;  // RGBA
      default:
        return std::nullopt;
    }
    srcStride = width * srcBpp;
  }

  // Decompress.
  std::size_t rawSize =
      static_cast<std::size_t>(height) * (1 + static_cast<std::size_t>(srcStride));
  std::vector<std::uint8_t> raw(rawSize);

  uLongf destLen = static_cast<uLongf>(rawSize);
  int ret = uncompress(raw.data(), &destLen, compressedData.data(),
                       static_cast<uLong>(compressedData.size()));
  if (ret != Z_OK || destLen != rawSize) {
    return std::nullopt;
  }

  // Unfilter scanlines and convert to RGBA.
  std::vector<std::uint8_t> prevRow(srcStride, 0);
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);

  for (std::uint32_t y = 0; y < height; ++y) {
    std::size_t scanlineStart = static_cast<std::size_t>(y) * (1 + srcStride);
    std::uint8_t filterType = raw[scanlineStart];
    std::uint8_t* rowData = &raw[scanlineStart + 1];

    if (!unfilterScanline(rowData, prevRow.data(), srcStride, filterType,
                          srcBpp)) {
      return std::nullopt;
    }

    // Convert to RGBA.
    std::uint8_t* dst = &rgba[static_cast<std::size_t>(y) * width * 4];
    if (colorType == 3) {
      // Indexed: unpack sub-byte pixels and look up palette.
      std::uint32_t paletteEntries = static_cast<std::uint32_t>(palette.size() / 3);
      for (std::uint32_t x = 0; x < width; ++x) {
        // Extract the palette index from packed bits.
        std::uint32_t bitOffset = x * bitDepth;
        std::uint32_t byteIdx = bitOffset / 8;
        std::uint32_t bitShift = 8 - bitDepth - (bitOffset % 8);
        std::uint8_t mask = static_cast<std::uint8_t>((1u << bitDepth) - 1);
        std::uint8_t idx = (rowData[byteIdx] >> bitShift) & mask;

        if (idx < paletteEntries) {
          dst[x * 4 + 0] = palette[idx * 3 + 0];
          dst[x * 4 + 1] = palette[idx * 3 + 1];
          dst[x * 4 + 2] = palette[idx * 3 + 2];
          dst[x * 4 + 3] = (idx < trnsData.size()) ? trnsData[idx] : 255;
        } else {
          dst[x * 4 + 0] = 0;
          dst[x * 4 + 1] = 0;
          dst[x * 4 + 2] = 0;
          dst[x * 4 + 3] = 255;
        }
      }
    } else {
      for (std::uint32_t x = 0; x < width; ++x) {
        switch (colorType) {
          case 0:  // Grayscale -> RGBA
            dst[x * 4 + 0] = rowData[x];
            dst[x * 4 + 1] = rowData[x];
            dst[x * 4 + 2] = rowData[x];
            dst[x * 4 + 3] = 255;
            break;
          case 2:  // RGB -> RGBA
            dst[x * 4 + 0] = rowData[x * 3 + 0];
            dst[x * 4 + 1] = rowData[x * 3 + 1];
            dst[x * 4 + 2] = rowData[x * 3 + 2];
            dst[x * 4 + 3] = 255;
            break;
          case 4:  // Grayscale+Alpha -> RGBA
            dst[x * 4 + 0] = rowData[x * 2];
            dst[x * 4 + 1] = rowData[x * 2];
            dst[x * 4 + 2] = rowData[x * 2];
            dst[x * 4 + 3] = rowData[x * 2 + 1];
            break;
          case 6:  // RGBA -> RGBA (direct copy)
            dst[x * 4 + 0] = rowData[x * 4 + 0];
            dst[x * 4 + 1] = rowData[x * 4 + 1];
            dst[x * 4 + 2] = rowData[x * 4 + 2];
            dst[x * 4 + 3] = rowData[x * 4 + 3];
            break;
        }
      }
    }

    std::copy(rowData, rowData + srcStride, prevRow.begin());
  }

  return DecodedPng{width, height, std::move(rgba)};
}

}  // namespace tiny_skia::test_utils
