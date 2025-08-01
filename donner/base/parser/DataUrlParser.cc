#include "donner/base/parser/DataUrlParser.h"

#include "donner/base/StringUtils.h"
#include "donner/base/encoding/Base64.h"
#include "donner/base/encoding/UrlDecode.h"

namespace donner::parser {

std::variant<DataUrlParser::Result, DataUrlParserError> DataUrlParser::Parse(std::string_view uri) {
  Result result;

  // If the URI is of format "data:image/png;base64,...", it is a data URL.
  constexpr std::string_view dataPrefix = "data:";
  if (StringUtils::StartsWith(uri, dataPrefix)) {
    std::string_view remaining = uri.substr(dataPrefix.size());

    result.kind = Result::Kind::Data;

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
        result.payload = std::move(maybeLoadedData.result());
      } else {
        return DataUrlParserError::InvalidDataUrl;
      }
    } else {
      // No "base64," prefix found, decode as URL-encoded data.
      result.payload = UrlDecode(remaining);
    }

  } else {
    // Assume it's a file path or URL.
    result.kind = Result::Kind::ExternalUrl;
    result.payload = RcString(uri);
  }

  return result;
}

}  // namespace donner::parser
