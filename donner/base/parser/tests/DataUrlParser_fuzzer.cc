#include <cstdlib>

#include "donner/base/parser/DataUrlParser.h"

namespace donner::parser {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
///
/// Exercises data: URL parsing (base64 and percent-encoded payloads) and plain external URL
/// passthrough, both of which are reachable from untrusted SVG/CSS `url(...)` and `xlink:href`
/// attribute values.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE: Allow reinterpret_cast
  const std::string_view buffer(reinterpret_cast<const char*>(data), size);

  auto result = DataUrlParser::Parse(buffer);
  if (std::holds_alternative<DataUrlParser::Result>(result)) {
    const DataUrlParser::Result& parsed = std::get<DataUrlParser::Result>(result);
    if (parsed.kind == DataUrlParser::Result::Kind::Data) {
      if (!std::holds_alternative<std::vector<uint8_t>>(parsed.payload)) {
        std::abort();
      }
    } else {
      if (!std::holds_alternative<RcString>(parsed.payload)) {
        std::abort();
      }
    }
  }

  DataUrlParser::Options limitedOptions;
  limitedOptions.maximumInputSize = size / 2;
  auto limitedResult = DataUrlParser::Parse(buffer, limitedOptions);
  if (size > limitedOptions.maximumInputSize &&
      (!std::holds_alternative<DataUrlParserError>(limitedResult) ||
       std::get<DataUrlParserError>(limitedResult) != DataUrlParserError::InputTooLarge)) {
    std::abort();
  }

  return 0;
}

}  // namespace donner::parser
