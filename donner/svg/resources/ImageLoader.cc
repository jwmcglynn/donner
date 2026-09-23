#include "donner/svg/resources/ImageLoader.h"

#include <stb/stb_image.h>

#include <algorithm>
#include <limits>
#include <optional>

namespace donner::svg {

namespace {

constexpr int kMaxImageDimension = 16384;

/// Detect if file contents look like SVG (XML starting with '<') or SVGZ (gzip magic bytes).
bool LooksLikeSvgContent(const std::vector<uint8_t>& data) {
  // Skip leading whitespace.
  size_t i = 0;
  while (i < data.size() &&
         (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n')) {
    ++i;
  }
  if (i >= data.size()) {
    return false;
  }
  // Check for XML start tag.
  if (data[i] == '<') {
    return true;
  }
  // Check for gzip magic bytes (SVGZ).
  if (data.size() >= 2 && data[0] == 0x1F && data[1] == 0x8B) {
    return true;
  }
  return false;
}

/// Returns true if \p data begins with the PNG magic bytes.
bool IsPng(const std::vector<uint8_t>& data) {
  // PNG: 89 50 4E 47 0D 0A 1A 0A
  return data.size() >= 8 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E &&
         data[3] == 0x47 && data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A &&
         data[7] == 0x0A;
}

/// Returns true if every declared PNG chunk payload fits within the input.
///
/// stb_image sizes its IDAT accumulation buffer from each chunk's declared
/// length before it checks that the payload bytes are present, so a truncated
/// PNG can request a multi-gigabyte allocation from a tiny input
/// (memory-exhaustion decode bomb). Walk the chunk headers and reject any
/// declared length the input cannot supply. Malformed headers are left to
/// stb_image's own validation.
bool PngDeclaredChunkLengthsFit(const std::vector<uint8_t>& data) {
  const auto readUint32BigEndian = [](const std::vector<uint8_t>& bytes, size_t offset) {
    return (static_cast<uint32_t>(bytes[offset]) << 24u) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16u) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8u) |
           static_cast<uint32_t>(bytes[offset + 3]);
  };

  // Layout: 8-byte signature, then [length:4][type:4][payload:length][crc:4].
  size_t offset = 8;
  while (offset + 8 <= data.size()) {
    const uint32_t declaredLength = readUint32BigEndian(data, offset);
    // The PNG specification caps chunk lengths at 2^31-1; reject larger values
    // before the size arithmetic below so it cannot overflow on 32-bit targets.
    if (declaredLength > 0x7FFFFFFFu) {
      return false;
    }
    const size_t available = data.size() - offset - 8;
    if (static_cast<size_t>(declaredLength) + 4u > available) {
      return false;  // Payload plus CRC cannot be present.
    }
    if (data[offset + 4] == 'I' && data[offset + 5] == 'E' && data[offset + 6] == 'N' &&
        data[offset + 7] == 'D') {
      return true;
    }
    offset += 12u + declaredLength;
  }
  return true;
}

/// Returns true if \p data begins with the magic bytes of a raster format we
/// intend to decode (PNG, JPEG, or GIF). stb_image also auto-detects magic-less
/// formats such as TGA whose decoders iterate the full declared width*height
/// regardless of how many input bytes are present, so a tiny input declaring
/// huge dimensions triggers a large decode loop (CPU-exhaustion DoS, hit
/// synchronously during document render). We only ever intend to accept the
/// three formats in the supported MIME list, so gate stb on their magic bytes.
bool HasSupportedRasterMagic(const std::vector<uint8_t>& data) {
  if (IsPng(data)) {
    return true;
  }
  // JPEG: FF D8 FF
  if (data.size() >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
    return true;
  }
  // GIF: "GIF87a" or "GIF89a"
  if (data.size() >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8' &&
      (data[4] == '7' || data[4] == '9') && data[5] == 'a') {
    return true;
  }
  return false;
}

std::optional<int> StbiInputLength(size_t byteCount) {
  if (byteCount > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }

  return static_cast<int>(byteCount);
}

std::optional<size_t> RgbaByteSize(int width, int height) {
  if (width <= 0 || height <= 0 || width > kMaxImageDimension || height > kMaxImageDimension) {
    return std::nullopt;
  }

  constexpr size_t kRgbaChannels = 4u;
  const size_t widthSize = static_cast<size_t>(width);
  const size_t heightSize = static_cast<size_t>(height);

  if (widthSize > std::numeric_limits<size_t>::max() / heightSize / kRgbaChannels) {
    return std::nullopt;
  }

  return widthSize * heightSize * kRgbaChannels;
}

size_t AmplificationBudget(size_t inputBytes, size_t maximumDecodedImageSize) {
  constexpr size_t kRatio = ImageLoader::kMaximumDecodedBytesPerInputByte;
  const size_t ratioBudget = inputBytes > std::numeric_limits<size_t>::max() / kRatio
                                 ? maximumDecodedImageSize
                                 : inputBytes * kRatio;
  return std::min(maximumDecodedImageSize,
                  std::max(ImageLoader::kMinimumDecodedImageAllowance, ratioBudget));
}

std::variant<ImageResource, UrlLoaderError> LoadImage(std::string_view mimeType,
                                                      const std::vector<uint8_t>& fileContents,
                                                      size_t maximumDecodedImageSize) {
  // Allow known formats and an empty mime type (stb_image will auto-detect)
  if (mimeType != "" && mimeType != "image/png" && mimeType != "image/jpeg" &&
      mimeType != "image/jpg" && mimeType != "image/gif") {
    return UrlLoaderError::UnsupportedFormat;
  }

  // Only hand stb inputs that begin with a supported format's magic bytes. This
  // rejects magic-less formats (e.g. TGA) whose decoders would otherwise run a
  // full width*height loop on a tiny declared-huge input (decode-bomb DoS). This
  // is a raster-path decode failure, not an unsupported MIME type, so it returns
  // DataCorrupt to match the loader's error contract.
  if (!HasSupportedRasterMagic(fileContents)) {
    return UrlLoaderError::DataCorrupt;
  }

  // stb_image sizes its IDAT accumulation buffer from each chunk's declared
  // length before it verifies the payload is present, so a truncated PNG can
  // request a multi-gigabyte allocation from a tiny input (memory-exhaustion
  // DoS, hit synchronously during document render). Validate the declared
  // chunk lengths against the bytes actually supplied before stb runs.
  if (IsPng(fileContents) && !PngDeclaredChunkLengthsFit(fileContents)) {
    return UrlLoaderError::DataCorrupt;
  }

  const std::optional<int> inputLength = StbiInputLength(fileContents.size());
  if (!inputLength.has_value()) {
    return UrlLoaderError::DataCorrupt;
  }

  int width = 0;
  int height = 0;
  int channels = 0;
  if (stbi_info_from_memory(reinterpret_cast<const unsigned char*>(
                                fileContents.data()),  // NOLINT, allow reinterpret_cast.
                            *inputLength, &width, &height, &channels) == 0) {
    return UrlLoaderError::DataCorrupt;
  }

  const std::optional<size_t> dataSize = RgbaByteSize(width, height);
  if (!dataSize.has_value()) {
    return UrlLoaderError::DataCorrupt;
  }
  const size_t decodedLimit = AmplificationBudget(fileContents.size(), maximumDecodedImageSize);
  if (*dataSize > decodedLimit) {
    return UrlLoaderError::ResourceTooLarge;
  }

  uint8_t* data =
      stbi_load_from_memory(reinterpret_cast<const unsigned char*>(
                                fileContents.data()),  // NOLINT, allow reinterpret_cast.
                            *inputLength, &width, &height, &channels, 4);
  if (!data) {
    return UrlLoaderError::DataCorrupt;
  }
  const std::optional<size_t> loadedDataSize = RgbaByteSize(width, height);
  if (!loadedDataSize.has_value()) {
    stbi_image_free(data);
    return UrlLoaderError::DataCorrupt;
  }
  if (*loadedDataSize > decodedLimit) {
    stbi_image_free(data);
    return UrlLoaderError::ResourceTooLarge;
  }

  ImageResource result;
  result.data = std::vector<uint8_t>(data, data + *loadedDataSize);
  result.width = width;
  result.height = height;

  stbi_image_free(data);

  return result;
}

}  // namespace

ImageLoader::Result ImageLoader::fromUri(std::string_view uri) {
  auto urlResultOrError = urlLoader_.fromUri(uri);
  if (std::holds_alternative<UrlLoaderError>(urlResultOrError)) {
    return std::get<UrlLoaderError>(urlResultOrError);
  }

  UrlLoader::Result& urlResult = std::get<UrlLoader::Result>(urlResultOrError);

  // Route SVG content to a separate path: return raw bytes for the caller to parse.
  // Also detect SVG when no MIME type is specified (e.g., `data:;base64,...`).
  if (urlResult.mimeType == "image/svg+xml" ||
      (urlResult.mimeType.empty() && LooksLikeSvgContent(urlResult.data))) {
    return SvgImageContent{std::move(urlResult.data)};
  }

  const size_t remainingDecodedBytes =
      remainingResourceBytes_ != nullptr ? *remainingResourceBytes_ : maximumDecodedImageSize_;
  const size_t decodedLimit = std::min(maximumDecodedImageSize_, remainingDecodedBytes);
  auto rasterResult = LoadImage(urlResult.mimeType, urlResult.data, decodedLimit);
  if (std::holds_alternative<UrlLoaderError>(rasterResult)) {
    const UrlLoaderError error = std::get<UrlLoaderError>(rasterResult);
    if (error == UrlLoaderError::ResourceTooLarge && remainingResourceBytes_ != nullptr) {
      *remainingResourceBytes_ = 0;
    }
    return error;
  }

  ImageResource result = std::get<ImageResource>(std::move(rasterResult));
  if (remainingResourceBytes_ != nullptr) {
    // LoadImage checked this against the current budget before asking stb_image to allocate.
    *remainingResourceBytes_ -= result.data.size();
  }
  return result;
}

}  // namespace donner::svg
