#include "donner/base/encoding/Decompress.h"

#include <zlib.h>

namespace donner {

namespace {

/**
 * Helper function to decompress data using zlib.
 *
 * @param compressedData The data to decompress.
 * @param output The output vector to store the decompressed data.
 * @param windowBits The window bits to use for decompression, see zlib documentation for details.
 * @return \ref ParseResult with the output vector on success, or a \ref ParseError on failure.
 */
ParseResult<std::vector<uint8_t>> Inflate(std::string_view compressedData, int windowBits,
                                          std::optional<size_t> outputSize) {
  std::vector<uint8_t> output;
  if (outputSize) {
    output.resize(*outputSize);
  }

  z_stream stream;
  stream.zalloc = Z_NULL;
  stream.zfree = Z_NULL;
  stream.opaque = Z_NULL;

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): zlib API requires a non‑const pointer
  stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(compressedData.data()));
  stream.avail_in = static_cast<uInt>(compressedData.size());

  if (inflateInit2(&stream, windowBits) != Z_OK) {
    return ParseError("Failed to initialize zlib");
  }

  if (outputSize) {
    // If an output buffer is provided, decompress into it directly.
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());

    int ret = inflate(&stream, Z_FINISH);
    if (ret != Z_STREAM_END) {
      inflateEnd(&stream);
      ParseError err;
      err.reason = std::string("Failed to decompress zlib data: ") +
                   (stream.msg ? stream.msg : "Unknown error");
      return err;
    }

    if (stream.total_out != output.size()) {
      return ParseError("Zlib decompression size mismatch");
    }
  } else {
    // If no output buffer is provided, decompress in chunks.
    constexpr size_t kChunkSize = 16384;
    int ret = Z_OK;
    while (true) {
      output.resize(output.size() + kChunkSize);
      stream.next_out = reinterpret_cast<Bytef*>(output.data() + output.size() - kChunkSize);
      stream.avail_out = kChunkSize;

      ret = inflate(&stream, Z_NO_FLUSH);

      if (ret == Z_STREAM_END) {
        output.resize(output.size() - stream.avail_out);
        break;
      }

      if (ret != Z_OK) {
        inflateEnd(&stream);
        ParseError err;
        err.reason = std::string("Failed to decompress gzip data: ") +
                     (stream.msg ? stream.msg : "Unknown error");
        return err;
      }
    }
  }

  inflateEnd(&stream);
  return output;
}

}  // namespace

ParseResult<std::vector<uint8_t>> Decompress::Gzip(std::string_view compressedData) {
  if (compressedData.size() < 2) {
    return ParseError("Gzip data is too short");
  }

  const unsigned char* data = reinterpret_cast<const unsigned char*>(compressedData.data());
  if (!(data[0] == 0x1f && data[1] == 0x8b)) {
    // Not gzip data.
    return ParseError("Invalid gzip header");
  }

  // 16 + MAX_WBITS enables gzip decoding.
  return Inflate(compressedData, 16 + MAX_WBITS, std::nullopt);
}

ParseResult<std::vector<uint8_t>> Decompress::Zlib(std::string_view compressedData,
                                                   size_t decompressedSize) {
  return Inflate(compressedData, MAX_WBITS, decompressedSize);
}

}  // namespace donner
