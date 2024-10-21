#include "donner/svg/parser/SVGParser.h"

namespace donner::svg::parser {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE: Allow reinterpret_cast
  const std::string_view buffer(reinterpret_cast<const char*>(data), size);
  auto result = SVGParser::ParseSVG(buffer);
  (void)result;

  // Try again with options set.
  SVGParser::Options options;
  options.disableUserAttributes = false;

  auto result2 = SVGParser::ParseSVG(buffer, nullptr, options);
  (void)result2;

  return 0;
}

}  // namespace donner::svg::parser
