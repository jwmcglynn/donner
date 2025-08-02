#include "donner/base/parser/DataUrlParser.h"

#include "donner/base/StringUtils.h"
#include "donner/base/encoding/Base64.h"
#include "donner/base/encoding/UrlDecode.h"

namespace donner::parser {

std::variant<DataUrlParser::Result, DataUrlParserError> DataUrlParser::Parse(std::string_view uri) {
  Result result;

  // If the URI is of format "data:image/png;base64,...", it is a data URL.
  constexpr std::string_view dataPrefix = "data:";
  if (!StringUtils::StartsWith(uri, dataPrefix)) {
    result.kind = Result::Kind::ExternalUrl;
    result.payload = RcString(uri);
    return result;
  }

  std::string_view remaining = uri.substr(dataPrefix.size());

  result.kind = Result::Kind::Data;

  const size_t commaPos = remaining.find(',');
  if (commaPos == std::string_view::npos) {
    return DataUrlParserError::InvalidDataUrl;
  }

  const std::string_view metadata = remaining.substr(0, commaPos);
  std::string_view payload = remaining.substr(commaPos + 1);

  std::string_view mimeTypePart = metadata;
  bool isBase64 = false;
  if (metadata.ends_with(";base64")) {
    isBase64 = true;
    mimeTypePart.remove_suffix(7);
  }

  if (!mimeTypePart.empty()) {
    if (const size_t semicolonPos = mimeTypePart.find(';');
        semicolonPos != std::string_view::npos) {
      result.mimeType = std::string(mimeTypePart.substr(0, semicolonPos));
    } else {
      result.mimeType = std::string(mimeTypePart);
    }
  }

  if (isBase64) {
    auto maybeLoadedData = DecodeBase64Data(payload);
    if (maybeLoadedData.hasResult()) {
      result.payload = std::move(maybeLoadedData.result());
    } else {
      return DataUrlParserError::InvalidDataUrl;
    }
  } else {
    result.payload = UrlDecode(payload);
  }

  return result;
}

}  // namespace donner::parser
