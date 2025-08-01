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
    // It's an external URL, fetch it.
    auto maybeLoadedData =
        resourceLoader_.fetchExternalResource(std::get<RcString>(parsedUrl.payload));
    if (std::holds_alternative<ResourceLoaderError>(maybeLoadedData)) {
      return MapError(std::get<ResourceLoaderError>(maybeLoadedData));
    }

    result.data = std::get<std::vector<uint8_t>>(maybeLoadedData);
  }

  return result;
}

}  // namespace donner::svg
