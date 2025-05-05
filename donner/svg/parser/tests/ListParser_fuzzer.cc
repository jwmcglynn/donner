#include <string_view>

#include "donner/svg/parser/ListParser.h"

namespace donner::svg::parser {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE: Allow reinterpret_cast
  const std::string_view buffer(reinterpret_cast<const char*>(data), size);

  // We don't need to store the results, just call the callback.
  ListParser::Parse(buffer, [](std::string_view /*item*/) {
    // No-op callback
  });

  return 0;
}

}  // namespace donner::svg::parser
