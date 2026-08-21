#include "donner/svg/resources/UrlLoader.h"

#include "donner/base/StringUtils.h"
#include "donner/base/Utf8.h"
#include "donner/base/parser/DataUrlParser.h"

namespace donner::svg {

static_assert(UrlLoader::kMaximumExternalUriSize ==
              parser::DataUrlParser::kDefaultMaximumExternalUrlSize);

using parser::DataUrlParser;
using parser::DataUrlParserError;

namespace {

UrlLoaderError MapError(ResourceLoaderError error) {
  switch (error) {
    case ResourceLoaderError::TooLarge: return UrlLoaderError::ResourceTooLarge;
    case ResourceLoaderError::NotFound:
    case ResourceLoaderError::SandboxViolation: return UrlLoaderError::NotFound;
  }

  UTILS_UNREACHABLE();
}

UrlLoaderError MapError(DataUrlParserError error) {
  switch (error) {
    case DataUrlParserError::InvalidDataUrl: return UrlLoaderError::InvalidDataUrl;
    case DataUrlParserError::InputTooLarge: return UrlLoaderError::ResourceTooLarge;
  }

  UTILS_UNREACHABLE();
}

/// Detect MIME type from a URL's file extension. Returns an empty string for unknown extensions.
std::string MimeTypeFromUrl(std::string_view url) {
  // Find the last '.' in the URL, ignoring any query string or fragment.
  const size_t queryPos = url.find_first_of("?#");
  const std::string_view path = url.substr(0, queryPos);
  const size_t dotPos = path.rfind('.');
  if (dotPos == std::string_view::npos) {
    return "";
  }

  const std::string_view ext = path.substr(dotPos + 1);

  using namespace std::string_view_literals;

  if (StringUtils::EqualsLowercase(ext, "svg"sv)) {
    return "image/svg+xml";
  } else if (StringUtils::EqualsLowercase(ext, "svgz"sv)) {
    return "image/svg+xml";
  } else if (StringUtils::EqualsLowercase(ext, "png"sv)) {
    return "image/png";
  } else if (StringUtils::EqualsLowercase(ext, "jpg"sv) ||
             StringUtils::EqualsLowercase(ext, "jpeg"sv)) {
    return "image/jpeg";
  } else if (StringUtils::EqualsLowercase(ext, "gif"sv)) {
    return "image/gif";
  } else if (StringUtils::EqualsLowercase(ext, "webp"sv)) {
    return "image/webp";
  }

  return "";
}

bool ConsumeResourceBytes(size_t size, size_t maximumResourceSize, size_t* remainingResourceBytes) {
  if (size > maximumResourceSize ||
      (remainingResourceBytes != nullptr && size > *remainingResourceBytes)) {
    if (remainingResourceBytes != nullptr) {
      *remainingResourceBytes = 0;
    }
    return false;
  }

  if (remainingResourceBytes != nullptr) {
    *remainingResourceBytes -= size;
  }
  return true;
}

void LatchAggregateRejection(UrlLoaderError error, size_t* remainingResourceBytes) {
  if (error == UrlLoaderError::ResourceTooLarge && remainingResourceBytes != nullptr) {
    *remainingResourceBytes = 0;
  }
}

std::variant<UrlLoader::Result, UrlLoaderError> LoadDataUrl(DataUrlParser::Result& parsedUrl,
                                                            size_t maximumResourceSize,
                                                            size_t* remainingResourceBytes) {
  UrlLoader::Result result;
  result.data = std::move(std::get<std::vector<uint8_t>>(parsedUrl.payload));
  if (!ConsumeResourceBytes(result.data.size(), maximumResourceSize, remainingResourceBytes)) {
    return UrlLoaderError::ResourceTooLarge;
  }
  result.mimeType = parsedUrl.mimeType;
  return result;
}

std::variant<UrlLoader::Result, UrlLoaderError> LoadExternalUrl(
    DataUrlParser::Result& parsedUrl, ResourceLoaderInterface& resourceLoader,
    size_t maximumResourceSize, size_t* remainingResourceBytes) {
  if (maximumResourceSize == 0 ||
      (remainingResourceBytes != nullptr && *remainingResourceBytes == 0)) {
    return UrlLoaderError::ResourceTooLarge;
  }

  const RcString& url = std::get<RcString>(parsedUrl.payload);
  auto maybeLoadedData = resourceLoader.fetchExternalResource(url);
  if (std::holds_alternative<ResourceLoaderError>(maybeLoadedData)) {
    const UrlLoaderError error = MapError(std::get<ResourceLoaderError>(maybeLoadedData));
    LatchAggregateRejection(error, remainingResourceBytes);
    return error;
  }

  UrlLoader::Result result;
  result.data = std::get<std::vector<uint8_t>>(std::move(maybeLoadedData));
  if (!ConsumeResourceBytes(result.data.size(), maximumResourceSize, remainingResourceBytes)) {
    return UrlLoaderError::ResourceTooLarge;
  }
  result.mimeType = MimeTypeFromUrl(url);
  return result;
}

}  // namespace

std::optional<UrlLoaderError> UrlLoader::validateExternalUriRepresentation(std::string_view uri) {
  if (uri.size() > kMaximumExternalUriSize) {
    return UrlLoaderError::ResourceTooLarge;
  }
  if (uri.find('\0') != std::string_view::npos || !Utf8::IsValidString(uri)) {
    return UrlLoaderError::InvalidDataUrl;
  }
  return std::nullopt;
}

std::variant<UrlLoader::Result, UrlLoaderError> UrlLoader::fromUri(std::string_view uri) {
  if (remainingResourceBytes_ != nullptr && *remainingResourceBytes_ == 0) {
    return UrlLoaderError::ResourceTooLarge;
  }

  // Reject attacker-sized external identifiers before DataUrlParser copies them into RcString and
  // before caching loaders copy/hash them again. Data URLs retain the larger encoded-input limit
  // because their payload is the resource itself and is bounded again after decoding.
  if (!uri.starts_with("data:")) {
    if (const auto representationError = validateExternalUriRepresentation(uri)) {
      return *representationError;
    }
  }

  std::variant<DataUrlParser::Result, DataUrlParserError> maybeParsedUrl =
      DataUrlParser::Parse(uri);

  if (std::holds_alternative<DataUrlParserError>(maybeParsedUrl)) {
    const UrlLoaderError error = MapError(std::get<DataUrlParserError>(maybeParsedUrl));
    LatchAggregateRejection(error, remainingResourceBytes_);
    return error;
  }

  DataUrlParser::Result& parsedUrl = std::get<DataUrlParser::Result>(maybeParsedUrl);
  if (parsedUrl.kind == DataUrlParser::Result::Kind::Data) {
    return LoadDataUrl(parsedUrl, maximumResourceSize_, remainingResourceBytes_);
  }
  return LoadExternalUrl(parsedUrl, resourceLoader_, maximumResourceSize_, remainingResourceBytes_);
}

}  // namespace donner::svg
