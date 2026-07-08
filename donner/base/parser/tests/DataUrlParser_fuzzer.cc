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
      assert(std::holds_alternative<std::vector<uint8_t>>(parsed.payload) &&
             "Data URL result should carry a byte payload");
    } else {
      assert(std::holds_alternative<RcString>(parsed.payload) &&
             "External URL result should carry a string payload");
    }
  }

  return 0;
}

}  // namespace donner::parser
