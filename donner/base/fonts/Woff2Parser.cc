#include "donner/base/fonts/Woff2Parser.h"

#include <woff2/decode.h>
#include <woff2/output.h>

namespace donner::fonts {
namespace {

constexpr size_t kWoff2HeaderSize = 48;
// Bound work delegated to woff2 for untrusted compressed input.
constexpr size_t kMaxWoff2InputSize = 64u * 1024u * 1024u;
constexpr size_t kMaxDecompressedSize = 64u * 1024u * 1024u;
constexpr uint32_t kWoff2Signature = 0x774F4632;  // "wOF2"

uint16_t ReadBigEndianU16(std::span<const uint8_t> data, size_t offset) {
  return (static_cast<uint16_t>(data[offset]) << 8) | static_cast<uint16_t>(data[offset + 1]);
}

uint32_t ReadBigEndianU32(std::span<const uint8_t> data, size_t offset) {
  return (static_cast<uint32_t>(data[offset]) << 24) |
         (static_cast<uint32_t>(data[offset + 1]) << 16) |
         (static_cast<uint32_t>(data[offset + 2]) << 8) | static_cast<uint32_t>(data[offset + 3]);
}

}  // namespace

ParseResult<std::vector<uint8_t>> Woff2Parser::Decompress(std::span<const uint8_t> woff2Data) {
  if (woff2Data.size() > kMaxWoff2InputSize) {
    ParseDiagnostic err;
    err.reason = "WOFF2: compressed input exceeds limit";
    return err;
  }

  if (woff2Data.size() < 4) {
    ParseDiagnostic err;
    err.reason = "WOFF2 data too short";
    return err;
  }

  if (ReadBigEndianU32(woff2Data, 0) != kWoff2Signature) {
    ParseDiagnostic err;
    err.reason = "WOFF2: invalid signature";
    return err;
  }

  if (woff2Data.size() < kWoff2HeaderSize) {
    ParseDiagnostic err;
    err.reason = "WOFF2: incomplete header";
    return err;
  }

  if (ReadBigEndianU32(woff2Data, 8) != woff2Data.size()) {
    ParseDiagnostic err;
    err.reason = "WOFF2: declared input length does not match data";
    return err;
  }

  if (ReadBigEndianU16(woff2Data, 12) == 0) {
    ParseDiagnostic err;
    err.reason = "WOFF2: header declares no tables";
    return err;
  }

  if (ReadBigEndianU16(woff2Data, 14) != 0) {
    ParseDiagnostic err;
    err.reason = "WOFF2: reserved header field must be zero";
    return err;
  }

  // Compute the decompressed output size from the WOFF2 header.
  const size_t outSize = woff2::ComputeWOFF2FinalSize(woff2Data.data(), woff2Data.size());
  if (outSize == 0) {
    ParseDiagnostic err;
    err.reason = "WOFF2: failed to compute decompressed size (invalid header)";
    return err;
  }

  // ComputeWOFF2FinalSize returns the attacker-controlled totalSfntSize header
  // field verbatim. Guard it before allocating: without this, a complete header
  // declaring a 4 GiB output triggers a multi-gigabyte allocation before any
  // decompression work. A legitimate decompressed font is far under this bound.
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
