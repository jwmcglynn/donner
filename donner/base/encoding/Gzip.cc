#include "donner/base/encoding/Gzip.h"

#include <zlib.h>

namespace donner {

ParseResult<std::vector<uint8_t>> DecompressGzip(std::string_view compressedData) {
  if (compressedData.size() < 2) {
    return ParseError("Gzip data is too short");
  }

  const unsigned char* data = reinterpret_cast<const unsigned char*>(compressedData.data());
  if (!(data[0] == 0x1f && data[1] == 0x8b)) {
    // Not gzip data.
    return ParseError("Invalid gzip header");
  }

  z_stream stream;
  stream.zalloc = Z_NULL;
  stream.zfree = Z_NULL;
  stream.opaque = Z_NULL;

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): zlib API requires a non‑const pointer
  stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
  stream.avail_in = static_cast<uInt>(compressedData.size());

  // 16 + MAX_WBITS enables gzip decoding.
  if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
    return ParseError("Failed to initialize zlib");
  }

  std::vector<uint8_t> output;
  constexpr size_t kChunkSize = 16384;
  int ret = Z_OK;
  while (true) {
    std::vector<uint8_t> out(kChunkSize);
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = kChunkSize;

    ret = inflate(&stream, Z_NO_FLUSH);

    if (ret == Z_STREAM_END) {
      const size_t bytes_written = kChunkSize - stream.avail_out;
      output.insert(output.end(), out.data(), out.data() + bytes_written);
      break;
    }

    if (ret != Z_OK) {
      inflateEnd(&stream);
      ParseError err;
      err.reason = std::string("Failed to decompress gzip data: ") +
                   (stream.msg ? stream.msg : "Unknown error");
      return err;
    }

    const size_t bytes_written = kChunkSize - stream.avail_out;
    output.insert(output.end(), out.data(), out.data() + bytes_written);
  }

  inflateEnd(&stream);
  return output;
}

}  // namespace donner
