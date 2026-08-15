#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/base/Utils.h"

namespace donner::parser {

/**
 * Enum of possible errors that can occur when loading an image.
 */
enum class DataUrlParserError : uint8_t {
  InvalidDataUrl,  ///< The data URL is invalid.
  InputTooLarge,   ///< The URI exceeds the configured input limit.
};

/// Returns a human-readable description of a \ref DataUrlParserError.
inline std::string_view ToString(DataUrlParserError err) {
  switch (err) {
    case DataUrlParserError::InvalidDataUrl: return "Invalid data URL";
    case DataUrlParserError::InputTooLarge: return "Data URL input too large";
  }

  UTILS_UNREACHABLE();
}

/**
 * Parse a URL, which can be an external resource or a data URL.
 *
 * A data URL will be parsed into the raw data it contains, while an external URL will be
 * returned as a string.
 */
class DataUrlParser {
public:
  /// Default maximum URI length accepted from untrusted input.
  static constexpr size_t kDefaultMaximumInputSize = 16 * 1024 * 1024;

  struct Options {
    /// Maximum number of encoded URI bytes accepted before decoding.
    size_t maximumInputSize = kDefaultMaximumInputSize;
  };

  /**
   * Result of parsing a data URL or external URL.
   */
  struct Result {
    /// The payload of the source, which can be a URL or raw data (already parsed from the data
    /// URL).
    enum class Kind : uint8_t {
      ExternalUrl,  ///< A file path or URL, \ref payload contains an RcString.
      Data  ///< A data URL which has been parsed, \ref payload contains the raw data as a vector.
    };

    /// What kind of URL this is
    Kind kind;

    /// MIME type of the data, if known. Otherwise, an empty string.
    std::string mimeType;

    /// The payload of the source, which can be a URL or raw data (already parsed from the data
    /// URL).
    std::variant<RcString, std::vector<uint8_t>> payload;
  };

  /**
   * Parse a URL, which can be an external resource or a data URL.
   *
   * @param uri The URL-encoded string to parse.
   * @return Result containing the parsed URL kind and payload, or an error if the input is not
   * valid.
   */
  static std::variant<Result, DataUrlParserError> Parse(std::string_view uri);

  /// Parse with an explicit encoded-input limit.
  static std::variant<Result, DataUrlParserError> Parse(std::string_view uri,
                                                        const Options& options);
};

}  // namespace donner::parser
