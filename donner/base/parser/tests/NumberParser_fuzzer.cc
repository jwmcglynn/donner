#include <cmath>  // for std::isnan

#include "donner/base/parser/NumberParser.h"

namespace donner::parser {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE: Allow reinterpret_cast
  const std::string_view buffer(reinterpret_cast<const char*>(data), size);

  auto result = NumberParser::Parse(buffer);
  if (result.hasResult()) {
    assert(!std::isnan(result.result().number) && "Final value should not be NaN");
  }

  return 0;
}

}  // namespace donner::parser
