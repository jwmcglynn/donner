#include "donner/svg/resources/UrlLoader.h"

#include "donner/base/StringUtils.h"
#include "donner/base/parser/DataUrlParser.h"

namespace donner::svg {

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

}  // namespace

bool UrlLoader::consumeResourceBytes(size_t size) {
  if (size > maximumResourceSize_ ||
      (remainingResourceBytes_ != nullptr && size > *remainingResourceBytes_)) {
    if (remainingResourceBytes_ != nullptr) {
      *remainingResourceBytes_ = 0;
    }
    return false;
  }

  if (remainingResourceBytes_ != nullptr) {
    *remainingResourceBytes_ -= size;
  }
  return true;
}

std::variant<UrlLoader::Result, UrlLoaderError> UrlLoader::fromUri(std::string_view uri) {
  if (remainingResourceBytes_ != nullptr && *remainingResourceBytes_ == 0) {
    return UrlLoaderError::ResourceTooLarge;
  }

  Result result;

  std::variant<DataUrlParser::Result, DataUrlParserError> maybeParsedUrl =
      DataUrlParser::Parse(uri);

  if (std::holds_alternative<DataUrlParserError>(maybeParsedUrl)) {
    const UrlLoaderError error = MapError(std::get<DataUrlParserError>(maybeParsedUrl));
    if (error == UrlLoaderError::ResourceTooLarge && remainingResourceBytes_ != nullptr) {
      *remainingResourceBytes_ = 0;
    }
    return error;
  }

  DataUrlParser::Result& parsedUrl = std::get<DataUrlParser::Result>(maybeParsedUrl);

  if (parsedUrl.kind == DataUrlParser::Result::Kind::Data) {
    result.data = std::move(std::get<std::vector<uint8_t>>(parsedUrl.payload));
    if (!consumeResourceBytes(result.data.size())) {
      return UrlLoaderError::ResourceTooLarge;
    }
    result.mimeType = parsedUrl.mimeType;
    return result;
  } else {
    const RcString& url = std::get<RcString>(parsedUrl.payload);

    // It's an external URL, fetch it.
    auto maybeLoadedData = resourceLoader_.fetchExternalResource(url);
    if (std::holds_alternative<ResourceLoaderError>(maybeLoadedData)) {
      const UrlLoaderError error = MapError(std::get<ResourceLoaderError>(maybeLoadedData));
      if (error == UrlLoaderError::ResourceTooLarge && remainingResourceBytes_ != nullptr) {
        *remainingResourceBytes_ = 0;
      }
      return error;
    }

    result.data = std::get<std::vector<uint8_t>>(std::move(maybeLoadedData));
    if (!consumeResourceBytes(result.data.size())) {
      return UrlLoaderError::ResourceTooLarge;
    }
    result.mimeType = MimeTypeFromUrl(url);
  }

  return result;
}

}  // namespace donner::svg
