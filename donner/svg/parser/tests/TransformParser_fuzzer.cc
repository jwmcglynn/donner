#include <cmath>
#include <cstdlib>

#include "donner/svg/parser/TransformParser.h"

namespace donner::svg::parser {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto result = TransformParser::Parse(
      std::string_view(reinterpret_cast<const char*>(data),  // NOLINT: Intentional cast
                       size));
  // A successfully parsed transform must be finite.
  if (result.hasResult()) {
    for (double value : result.result().data) {
      if (!std::isfinite(value)) {
        std::abort();
      }
    }
  }

  return 0;
}

}  // namespace donner::svg::parser
