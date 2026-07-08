#include "donner/svg/resources/ImageLoader.h"

#include <stb/stb_image.h>

#include <limits>
#include <optional>

namespace donner::svg {

namespace {

constexpr int kMaxImageDimension = 16384;
constexpr size_t kMaxDecodedImageBytes = 256u * 1024u * 1024u;

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

/// Returns true if \p data begins with the magic bytes of a raster format we
/// intend to decode (PNG, JPEG, or GIF). stb_image also auto-detects magic-less
/// formats such as TGA whose decoders iterate the full declared width*height
/// regardless of how many input bytes are present, so a tiny input declaring
/// huge dimensions triggers a large decode loop (CPU-exhaustion DoS, hit
/// synchronously during document render). We only ever intend to accept the
/// three formats in the supported MIME list, so gate stb on their magic bytes.
bool HasSupportedRasterMagic(const std::vector<uint8_t>& data) {
  // PNG: 89 50 4E 47 0D 0A 1A 0A
  if (data.size() >= 8 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E &&
      data[3] == 0x47 && data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A) {
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

  if (widthSize > kMaxDecodedImageBytes / heightSize / kRgbaChannels) {
    return std::nullopt;
  }

  return widthSize * heightSize * kRgbaChannels;
}

std::variant<ImageResource, UrlLoaderError> LoadImage(std::string_view mimeType,
                                                      const std::vector<uint8_t>& fileContents) {
  // Allow known formats and an empty mime type (stb_image will auto-detect)
  if (mimeType != "" && mimeType != "image/png" && mimeType != "image/jpeg" &&
      mimeType != "image/jpg" && mimeType != "image/gif") {
    return UrlLoaderError::UnsupportedFormat;
  }

  // Only hand stb inputs that begin with a supported format's magic bytes. This
  // rejects magic-less formats (e.g. TGA) whose decoders would otherwise run a
  // full width*height loop on a tiny declared-huge input (decode-bomb DoS).
  if (!HasSupportedRasterMagic(fileContents)) {
    return UrlLoaderError::UnsupportedFormat;
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

  auto rasterResult = LoadImage(urlResult.mimeType, urlResult.data);
  if (std::holds_alternative<UrlLoaderError>(rasterResult)) {
    return std::get<UrlLoaderError>(rasterResult);
  }
  return std::get<ImageResource>(std::move(rasterResult));
}

}  // namespace donner::svg
