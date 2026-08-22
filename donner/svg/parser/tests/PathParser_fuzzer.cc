#include <cstdlib>
#include <string>

#include "donner/svg/parser/PathParser.h"

namespace donner::svg::parser {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data),  // NOLINT: Intentional cast
                               size);
  if (input.starts_with("path-command-budget")) {
    std::string pathData = "M0 0";
    pathData.reserve(PathParser::kMaximumCommands * 5);
    for (std::size_t i = 0; i < PathParser::kMaximumCommands; ++i) {
      pathData.append("L0 0");
    }

    const auto result = PathParser::Parse(pathData);
    if (!result.hasError() || !result.hasResult() ||
        result.result().commands().size() > PathParser::kMaximumCommands ||
        result.result().points().size() > PathParser::kMaximumPoints) {
      std::abort();
    }
  } else {
    const auto result = PathParser::Parse(input);
    (void)result;
  }

  return 0;
}

}  // namespace donner::svg::parser
