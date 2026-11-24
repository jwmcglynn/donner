#include <cmath>
#include <cstdlib>

#include "donner/svg/parser/Number2dParser.h"

namespace donner::svg::parser {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto result = Number2dParser::Parse(
      std::string_view(reinterpret_cast<const char*>(data),  // NOLINT: Intentional cast
                       size));
  // Out-of-range numbers are an error, so a successful result must be finite.
  if (result.hasResult() &&
      (!std::isfinite(result.result().numberX) || !std::isfinite(result.result().numberY))) {
    std::abort();
  }

  return 0;
}

}  // namespace donner::svg::parser
