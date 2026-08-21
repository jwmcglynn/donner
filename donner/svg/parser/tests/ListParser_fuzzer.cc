#include <cstdlib>
#include <string>
#include <string_view>

#include "donner/svg/parser/ListParser.h"

namespace donner::svg::parser {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE: Allow reinterpret_cast
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  std::string generated;
  const bool limitMarker = input.starts_with("list-item-budget");
  if (limitMarker) {
    generated.reserve((ListParser::kMaximumItems + 1) * 2);
    for (std::size_t i = 0; i <= ListParser::kMaximumItems; ++i) {
      generated += "a ";
    }
  }
  const std::string_view buffer = limitMarker ? std::string_view(generated) : input;

  std::size_t count = 0;
  const auto error = ListParser::Parse(buffer, [&](std::string_view /*item*/) { ++count; });
  if (count > ListParser::kMaximumItems || (limitMarker && !error.has_value())) {
    std::abort();
  }

  return 0;
}

}  // namespace donner::svg::parser
