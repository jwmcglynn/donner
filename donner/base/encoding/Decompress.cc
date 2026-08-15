#include "donner/base/encoding/Decompress.h"

#include <zlib.h>

#include <algorithm>
#include <limits>
#include <string>

namespace donner {

namespace {

/**
 * Helper function to decompress data using zlib.
 *
 * @param compressedData The data to decompress.
 * @param output The output vector to store the decompressed data.
 * @param windowBits The window bits to use for decompression, see zlib documentation for details.
 * @return \ref ParseResult with the output vector on success, or a \ref ParseDiagnostic on failure.
 */
ParseResult<std::vector<uint8_t>> Inflate(std::string_view compressedData, int windowBits,
                                          std::optional<size_t> outputSize,
                                          size_t maximumOutputSize) {
  if (compressedData.size() > std::numeric_limits<uInt>::max()) {
    return ParseDiagnostic::Error("Compressed data exceeds zlib input limit",
                                  FileOffset::Offset(0));
  }
  if (outputSize &&
      (*outputSize > maximumOutputSize || *outputSize > std::numeric_limits<uInt>::max())) {
    return ParseDiagnostic::Error("Requested output exceeds maximum decompressed size",
                                  FileOffset::Offset(0));
  }

  std::vector<uint8_t> output;
  if (outputSize) {
    output.resize(*outputSize);
  }

  z_stream stream;
  stream.zalloc = Z_NULL;
  stream.zfree = Z_NULL;
  stream.opaque = Z_NULL;

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): zlib API requires a non-const pointer
  stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(compressedData.data()));
  stream.avail_in = static_cast<uInt>(compressedData.size());

  if (inflateInit2(&stream, windowBits) != Z_OK) {
    return ParseDiagnostic::Error("Failed to initialize zlib", FileOffset::Offset(0));
  }

  if (outputSize) {
    // If an output buffer is provided, decompress into it directly.
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());

    int ret = inflate(&stream, Z_FINISH);
    if (ret != Z_STREAM_END) {
      const std::string message = stream.msg ? stream.msg : "Unknown error";
      inflateEnd(&stream);
      return ParseDiagnostic::Error(
          RcString(std::string("Failed to decompress zlib data: ") + message),
          FileOffset::Offset(0));
    }

    if (stream.total_out != output.size()) {
      inflateEnd(&stream);
      return ParseDiagnostic::Error("Zlib decompression size mismatch", FileOffset::Offset(0));
    }
  } else {
    // If no output buffer is provided, decompress in chunks.
    constexpr size_t kChunkSize = 16384;
    int ret = Z_OK;
    while (true) {
      const size_t remaining = maximumOutputSize - output.size();
      if (remaining == 0) {
        // zlib can require one final call to consume the gzip trailer when the output buffer was
        // filled exactly. A one-byte probe distinguishes that from actual output beyond the cap.
        uint8_t overflowProbe = 0;
        stream.next_out = &overflowProbe;
        stream.avail_out = 1;
        ret = inflate(&stream, Z_NO_FLUSH);
        if (ret == Z_STREAM_END && stream.avail_out == 1) {
          break;
        }

        const std::string message = stream.msg ? stream.msg : "Unknown error";
        const bool producedOverflowByte = stream.avail_out == 0;
        inflateEnd(&stream);
        if (producedOverflowByte) {
          return ParseDiagnostic::Error("Gzip output exceeds maximum decompressed size",
                                        FileOffset::Offset(0));
        }
        return ParseDiagnostic::Error(
            RcString(std::string("Failed to decompress gzip data: ") + message),
            FileOffset::Offset(0));
      }

      const size_t chunkSize = std::min(kChunkSize, remaining);
      const size_t previousSize = output.size();
      output.resize(previousSize + chunkSize);
      stream.next_out = reinterpret_cast<Bytef*>(output.data() + previousSize);
      stream.avail_out = static_cast<uInt>(chunkSize);

      ret = inflate(&stream, Z_NO_FLUSH);
      output.resize(previousSize + chunkSize - stream.avail_out);

      if (ret == Z_STREAM_END) {
        break;
      }

      if (ret != Z_OK) {
        const std::string message = stream.msg ? stream.msg : "Unknown error";
        inflateEnd(&stream);
        return ParseDiagnostic::Error(
            RcString(std::string("Failed to decompress gzip data: ") + message),
            FileOffset::Offset(0));
      }
    }
  }

  inflateEnd(&stream);
  return output;
}

}  // namespace

ParseResult<std::vector<uint8_t>> Decompress::Gzip(std::string_view compressedData,
                                                   size_t maximumOutputSize) {
  if (compressedData.size() < 2) {
    return ParseDiagnostic::Error("Gzip data is too short", FileOffset::Offset(0));
  }

  const unsigned char* data = reinterpret_cast<const unsigned char*>(compressedData.data());
  if (!(data[0] == 0x1f && data[1] == 0x8b)) {
    // Not gzip data.
    return ParseDiagnostic::Error("Invalid gzip header", FileOffset::Offset(0));
  }

  // 16 + MAX_WBITS enables gzip decoding.
  return Inflate(compressedData, 16 + MAX_WBITS, std::nullopt, maximumOutputSize);
}

ParseResult<std::vector<uint8_t>> Decompress::Zlib(std::string_view compressedData,
                                                   size_t decompressedSize) {
  return Inflate(compressedData, MAX_WBITS, decompressedSize, decompressedSize);
}

}  // namespace donner
