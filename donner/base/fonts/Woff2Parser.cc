#include "donner/base/fonts/Woff2Parser.h"

#include <woff2/decode.h>
#include <woff2/output.h>

#include <optional>
#include <string>
#include <string_view>

namespace donner::fonts {
namespace {

constexpr size_t kWoff2HeaderSize = 48;
// Bound work delegated to woff2 for untrusted compressed input.
constexpr size_t kMaxWoff2InputSize = 16u * 1024u * 1024u;
constexpr size_t kMaxDecompressedSize = 64u * 1024u * 1024u;
// Bound the complete Brotli output buffer independently from the final sfnt writer.
constexpr size_t kMaxIntermediateSize = 16u * 1024u * 1024u;
// The pinned decoder can retain a 12-byte Point plus up to five glyph bytes for every transformed
// glyf-stream byte. Keeping that one table to 4 MiB bounds those table-derived scratch buffers to
// about 68 MiB before the decoder allocates them, without rejecting ordinary large non-glyf
// tables.
constexpr size_t kMaxTransformedGlyfSize = 4u * 1024u * 1024u;
constexpr size_t kMaxWoff2Tables = 4096;
constexpr size_t kMaxCollectionFonts = 256;
constexpr size_t kMaxCollectionTableReferences = 16384;
constexpr uint32_t kWoff2Signature = 0x774F4632;  // "wOF2"
constexpr uint32_t kTtcSignature = 0x74746366;    // "ttcf"
constexpr uint32_t kGlyfTag = 0x676C7966;         // "glyf"
constexpr uint32_t kLocaTag = 0x6C6F6361;         // "loca"

uint16_t ReadBigEndianU16(std::span<const uint8_t> data, size_t offset);
uint32_t ReadBigEndianU32(std::span<const uint8_t> data, size_t offset);

class Woff2Cursor {
public:
  explicit Woff2Cursor(std::span<const uint8_t> data) : data_(data) {}

  bool readU8(uint8_t* value) {
    if (offset_ >= data_.size()) {
      return false;
    }

    *value = data_[offset_++];
    return true;
  }

  bool readU16(uint16_t* value) {
    if (data_.size() - offset_ < 2) {
      return false;
    }

    *value = ReadBigEndianU16(data_, offset_);
    offset_ += 2;
    return true;
  }

  bool readU32(uint32_t* value) {
    if (data_.size() - offset_ < 4) {
      return false;
    }

    *value = ReadBigEndianU32(data_, offset_);
    offset_ += 4;
    return true;
  }

  bool skip(size_t count) {
    if (count > data_.size() - offset_) {
      return false;
    }

    offset_ += count;
    return true;
  }

  bool readBase128(uint32_t* value) {
    uint32_t result = 0;
    for (size_t i = 0; i < 5; ++i) {
      uint8_t code = 0;
      if (!readU8(&code) || (i == 0 && code == 0x80) || (result & 0xFE000000u) != 0) {
        return false;
      }

      result = (result << 7) | (code & 0x7Fu);
      if ((code & 0x80u) == 0) {
        *value = result;
        return true;
      }
    }

    return false;
  }

  bool read255UShort(uint32_t* value) {
    uint8_t code = 0;
    if (!readU8(&code)) {
      return false;
    }

    if (code == 253) {
      uint16_t result = 0;
      if (!readU16(&result)) {
        return false;
      }
      *value = result;
    } else if (code == 254 || code == 255) {
      uint8_t result = 0;
      if (!readU8(&result)) {
        return false;
      }
      *value = static_cast<uint32_t>(result) + (code == 255 ? 253u : 506u);
    } else {
      *value = code;
    }

    return true;
  }

private:
  std::span<const uint8_t> data_;
  size_t offset_ = 0;
};

uint16_t ReadBigEndianU16(std::span<const uint8_t> data, size_t offset) {
  return (static_cast<uint16_t>(data[offset]) << 8) | static_cast<uint16_t>(data[offset + 1]);
}

uint32_t ReadBigEndianU32(std::span<const uint8_t> data, size_t offset) {
  return (static_cast<uint32_t>(data[offset]) << 24) |
         (static_cast<uint32_t>(data[offset + 1]) << 16) |
         (static_cast<uint32_t>(data[offset + 2]) << 8) | static_cast<uint32_t>(data[offset + 3]);
}

std::optional<std::string_view> ValidateDecoderResourceBounds(std::span<const uint8_t> data) {
  const uint16_t numTables = ReadBigEndianU16(data, 12);
  if (numTables > kMaxWoff2Tables) {
    return "WOFF2: table count exceeds limit";
  }

  Woff2Cursor cursor(data);
  if (!cursor.skip(kWoff2HeaderSize)) {
    return "WOFF2: incomplete header";
  }

  size_t intermediateBytes = 0;
  for (uint16_t i = 0; i < numTables; ++i) {
    uint8_t flags = 0;
    if (!cursor.readU8(&flags)) {
      return "WOFF2: invalid table directory";
    }

    uint32_t tag = 0;
    const uint8_t knownTagIndex = flags & 0x3Fu;
    if (knownTagIndex == 0x3F) {
      if (!cursor.readU32(&tag)) {
        return "WOFF2: invalid table directory";
      }
    } else if (knownTagIndex == 10) {
      tag = kGlyfTag;
    } else if (knownTagIndex == 11) {
      tag = kLocaTag;
    }

    uint32_t originalLength = 0;
    if (!cursor.readBase128(&originalLength)) {
      return "WOFF2: invalid table directory";
    }

    const uint8_t transformVersion = flags >> 6;
    const bool transformed = ((tag == kGlyfTag || tag == kLocaTag) && transformVersion == 0) ||
                             ((tag != kGlyfTag && tag != kLocaTag) && transformVersion != 0);
    uint32_t transformLength = originalLength;
    if (transformed && !cursor.readBase128(&transformLength)) {
      return "WOFF2: invalid table directory";
    }
    if (tag == kLocaTag && transformed && transformLength != 0) {
      return "WOFF2: invalid table directory";
    }
    if (tag == kGlyfTag && transformed && transformLength > kMaxTransformedGlyfSize) {
      return "WOFF2: transformed glyf size exceeds limit";
    }

    if (transformLength > kMaxIntermediateSize - intermediateBytes) {
      return "WOFF2: intermediate decompressed size exceeds limit";
    }
    intermediateBytes += transformLength;
  }

  if (ReadBigEndianU32(data, 4) != kTtcSignature) {
    return std::nullopt;
  }

  uint32_t version = 0;
  uint32_t numFonts = 0;
  if (!cursor.readU32(&version) || (version != 0x00010000u && version != 0x00020000u) ||
      !cursor.read255UShort(&numFonts) || numFonts == 0) {
    return "WOFF2: invalid collection directory";
  }
  if (numFonts > kMaxCollectionFonts) {
    return "WOFF2: collection font count exceeds limit";
  }

  size_t tableReferences = 0;
  for (uint32_t i = 0; i < numFonts; ++i) {
    uint32_t fontTableCount = 0;
    uint32_t flavor = 0;
    if (!cursor.read255UShort(&fontTableCount) || fontTableCount == 0 || !cursor.readU32(&flavor)) {
      return "WOFF2: invalid collection directory";
    }
    if (fontTableCount > kMaxCollectionTableReferences - tableReferences) {
      return "WOFF2: collection table references exceed limit";
    }
    tableReferences += fontTableCount;

    for (uint32_t j = 0; j < fontTableCount; ++j) {
      uint32_t tableIndex = 0;
      if (!cursor.read255UShort(&tableIndex) || tableIndex >= numTables) {
        return "WOFF2: invalid collection directory";
      }
    }
  }

  return std::nullopt;
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

  if (const auto preflightError = ValidateDecoderResourceBounds(woff2Data)) {
    ParseDiagnostic err;
    err.reason = *preflightError;
    return err;
  }

  // Treat the declared size as a ceiling, not work that must happen before the stream is
  // validated. WOFF2StringOut grows only for bytes the decoder actually writes, while still
  // supporting the arbitrary-offset updates required for sfnt checksums and table metadata.
  std::string output;
  woff2::WOFF2StringOut out(&output);
  out.SetMaxSize(outSize);

  if (!woff2::ConvertWOFF2ToTTF(woff2Data.data(), woff2Data.size(), &out)) {
    ParseDiagnostic err;
    err.reason = "WOFF2: decompression failed";
    return err;
  }

  return std::vector<uint8_t>(output.begin(), output.end());
}

}  // namespace donner::fonts
