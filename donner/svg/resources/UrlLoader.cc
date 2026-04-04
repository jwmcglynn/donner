#include "donner/svg/resources/UrlLoader.h"

#include "donner/base/StringUtils.h"
#include "donner/base/parser/DataUrlParser.h"

namespace donner::svg {

using parser::DataUrlParser;
using parser::DataUrlParserError;

namespace {

UrlLoaderError MapError([[maybe_unused]] ResourceLoaderError error) {
  // Map all errors to NotFound for now.
  return UrlLoaderError::NotFound;
}

UrlLoaderError MapError(DataUrlParserError error) {
  switch (error) {
    case DataUrlParserError::InvalidDataUrl: return UrlLoaderError::InvalidDataUrl;
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

std::variant<UrlLoader::Result, UrlLoaderError> UrlLoader::fromUri(std::string_view uri) {
  Result result;

  std::variant<DataUrlParser::Result, DataUrlParserError> maybeParsedUrl =
      DataUrlParser::Parse(uri);

  if (std::holds_alternative<DataUrlParserError>(maybeParsedUrl)) {
    return MapError(std::get<DataUrlParserError>(maybeParsedUrl));
  }

  DataUrlParser::Result& parsedUrl = std::get<DataUrlParser::Result>(maybeParsedUrl);

  if (parsedUrl.kind == DataUrlParser::Result::Kind::Data) {
    result.data = std::move(std::get<std::vector<uint8_t>>(parsedUrl.payload));
    result.mimeType = parsedUrl.mimeType;
    return result;
  } else {
    const RcString& url = std::get<RcString>(parsedUrl.payload);

    // It's an external URL, fetch it.
    auto maybeLoadedData = resourceLoader_.fetchExternalResource(url);
    if (std::holds_alternative<ResourceLoaderError>(maybeLoadedData)) {
      return MapError(std::get<ResourceLoaderError>(maybeLoadedData));
    }

    result.data = std::get<std::vector<uint8_t>>(maybeLoadedData);
    result.mimeType = MimeTypeFromUrl(url);
  }

  return result;
}

}  // namespace donner::svg
