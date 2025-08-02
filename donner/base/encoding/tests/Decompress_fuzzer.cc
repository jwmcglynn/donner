#include "donner/base/encoding/Decompress.h"

namespace donner {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view strView(reinterpret_cast<const char*>(data), size);

  // Fuzz Gzip decompression
  auto resultGzip = Decompress::Gzip(strView);
  (void)resultGzip;

  // Fuzz Zlib decompression
  if (size > 0) {
    // Use the first byte as the decompressed size, to avoid OOMs.
    const size_t decompressedSize = data[0];
    auto resultZlib = Decompress::Zlib(strView, decompressedSize);
    (void)resultZlib;
  }

  return 0;
}

}  // namespace donner
