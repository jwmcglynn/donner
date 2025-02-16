#include "donner/svg/resources/UrlLoader.h"

#include "donner/base/StringUtils.h"
#include "donner/svg/resources/Base64.h"
#include "donner/svg/resources/UrlDecode.h"

namespace donner::svg {

namespace {

UrlLoaderError MapError([[maybe_unused]] ResourceLoaderError error) {
  // Map all errors to NotFound for now.
  return UrlLoaderError::NotFound;
}

}  // namespace

std::variant<UrlLoader::Result, UrlLoaderError> UrlLoader::fromUri(std::string_view uri) {
  Result result;

  // If the URI is of format "data:image/png;base64,...", it is a data URL.
  constexpr std::string_view dataPrefix = "data:";
  if (StringUtils::StartsWith(uri, dataPrefix)) {
    std::string_view remaining = uri.substr(dataPrefix.size());

    // Extract the mime type, until the first semicolon.
    if (const size_t mimeTypeEnd = remaining.find(';'); mimeTypeEnd != std::string::npos) {
      result.mimeType = remaining.substr(0, mimeTypeEnd);
      remaining.remove_prefix(result.mimeType.size() + 1);
    }

    // After the semicolon, look for a "base64," prefix
    constexpr std::string_view base64Prefix = "base64,";
    if (StringUtils::StartsWith(remaining, base64Prefix)) {
      remaining.remove_prefix(base64Prefix.size());

      auto maybeLoadedData = DecodeBase64Data(remaining);
      if (maybeLoadedData.hasResult()) {
        result.data = std::move(maybeLoadedData.result());
      } else {
        return UrlLoaderError::InvalidDataUrl;
      }
    } else {
      // No "base64," prefix found, decode as URL-encoded data.
      result.data = UrlDecode(remaining);
    }

  } else {
    // Assume it's a file path or URL.
    auto maybeLoadedData = resourceLoader_.fetchExternalResource(uri);
    if (std::holds_alternative<ResourceLoaderError>(maybeLoadedData)) {
      return MapError(std::get<ResourceLoaderError>(maybeLoadedData));
    }

    result.data = std::get<std::vector<uint8_t>>(maybeLoadedData);
  }

  return result;
}

}  // namespace donner::svg
