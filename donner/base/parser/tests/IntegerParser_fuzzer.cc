#include "donner/base/parser/IntegerParser.h"

namespace donner::parser {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE: Allow reinterpret_cast
  const std::string_view buffer(reinterpret_cast<const char*>(data), size);

  {
    auto result = IntegerParser::Parse(buffer);
    if (result.hasResult()) {
      // consumedChars must never exceed the input length.
      assert(result.result().consumedChars <= buffer.size() &&
             "consumedChars should not exceed input size");
    }
  }

  {
    auto result = IntegerParser::ParseHex(buffer);
    if (result.hasResult()) {
      assert(result.result().consumedChars <= buffer.size() &&
             "consumedChars should not exceed input size");
    }
  }

  return 0;
}

}  // namespace donner::parser
