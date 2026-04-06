#include "donner/base/fonts/Woff2Parser.h"

#include <woff2/decode.h>
#include <woff2/output.h>

namespace donner::fonts {

ParseResult<std::vector<uint8_t>> Woff2Parser::Decompress(std::span<const uint8_t> woff2Data) {
  if (woff2Data.size() < 4) {
    ParseError err;
    err.reason = "WOFF2 data too short";
    return err;
  }

  // Compute the decompressed output size from the WOFF2 header.
  const size_t outSize =
      woff2::ComputeWOFF2FinalSize(woff2Data.data(), woff2Data.size());
  if (outSize == 0) {
    ParseError err;
    err.reason = "WOFF2: failed to compute decompressed size (invalid header)";
    return err;
  }

  // Decompress into a pre-allocated buffer.
  std::vector<uint8_t> output(outSize);
  woff2::WOFF2MemoryOut out(output.data(), output.size());

  if (!woff2::ConvertWOFF2ToTTF(woff2Data.data(), woff2Data.size(), &out)) {
    ParseError err;
    err.reason = "WOFF2: decompression failed";
    return err;
  }

  // The actual output may be smaller than the header-declared size.
  output.resize(out.Size());
  return output;
}

}  // namespace donner::fonts
