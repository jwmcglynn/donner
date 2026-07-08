#include "donner/base/fonts/Woff2Parser.h"

#include <woff2/decode.h>
#include <woff2/output.h>

namespace donner::fonts {

ParseResult<std::vector<uint8_t>> Woff2Parser::Decompress(std::span<const uint8_t> woff2Data) {
  if (woff2Data.size() < 4) {
    ParseDiagnostic err;
    err.reason = "WOFF2 data too short";
    return err;
  }

  // Compute the decompressed output size from the WOFF2 header.
  const size_t outSize = woff2::ComputeWOFF2FinalSize(woff2Data.data(), woff2Data.size());
  if (outSize == 0) {
    ParseDiagnostic err;
    err.reason = "WOFF2: failed to compute decompressed size (invalid header)";
    return err;
  }

  // ComputeWOFF2FinalSize returns the attacker-controlled total_length header
  // field verbatim, with no validation against the input size. Guard against it
  // before allocating: without this, a 20-byte input declaring a 4 GiB size
  // triggers a multi-gigabyte allocation (hang / OOM DoS) well before any real
  // decompression work. A legitimate decompressed font is far under this bound.
  static constexpr size_t kMaxDecompressedSize = 64u * 1024u * 1024u;  // 64MB
  if (outSize > kMaxDecompressedSize) {
    ParseDiagnostic err;
    err.reason = "WOFF2: declared decompressed size exceeds limit";
    return err;
  }

  // Decompress into a pre-allocated buffer.
  std::vector<uint8_t> output(outSize);
  woff2::WOFF2MemoryOut out(output.data(), output.size());

  if (!woff2::ConvertWOFF2ToTTF(woff2Data.data(), woff2Data.size(), &out)) {
    ParseDiagnostic err;
    err.reason = "WOFF2: decompression failed";
    return err;
  }

  // The actual output may be smaller than the header-declared size.
  output.resize(out.Size());
  return output;
}

}  // namespace donner::fonts
