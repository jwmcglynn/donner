#include <cstdlib>
#include <string>

#include "donner/svg/parser/PointsListParser.h"

namespace donner::svg::parser {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);  // NOLINT
  const bool limitMarker = input.starts_with("points-item-budget");
  std::string generated;
  if (limitMarker) {
    generated.reserve((PointsListParser::kMaximumPoints + 1) * 4);
    for (std::size_t i = 0; i <= PointsListParser::kMaximumPoints; ++i) {
      generated += "0 0 ";
    }
  }
  auto result = PointsListParser::Parse(limitMarker ? std::string_view(generated) : input);
  if (result.hasResult() && result.result().size() > PointsListParser::kMaximumPoints) {
    std::abort();
  }
  if (limitMarker && !result.hasError()) {
    std::abort();
  }

  return 0;
}

}  // namespace donner::svg::parser
