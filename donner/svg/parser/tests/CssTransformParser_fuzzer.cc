#include "donner/svg/parser/CssTransformParser.h"

#include <string_view>

#include "donner/css/parser/ValueParser.h"

namespace donner::svg::parser {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE: Allow reinterpret_cast
  const std::string_view input(reinterpret_cast<const char*>(data), size);

  // Tokenize the fuzzed string the same way the CSS "transform" property value would be
  // tokenized before reaching CssTransformParser, e.g. "rotate(45deg) translate(1px, 2px)".
  std::vector<css::ComponentValue> components = css::parser::ValueParser::Parse(input);

  auto result = CssTransformParser::Parse(components);
  (void)result;

  return 0;
}

}  // namespace donner::svg::parser
