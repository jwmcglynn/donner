#include "donner/svg/xml/XMLParser.h"

namespace donner::svg::parser {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE: Allow reinterpret_cast
  XMLParser::InputBuffer buffer(std::string_view(reinterpret_cast<const char*>(data), size));
  auto result = XMLParser::ParseSVG(buffer);
  (void)result;

  // Try again with options set.
  XMLParser::Options options;
  options.disableUserAttributes = false;

  auto result2 = XMLParser::ParseSVG(buffer, nullptr, options);
  (void)result2;

  return 0;
}

}  // namespace donner::svg::parser
