#include <cmath>  // for std::isnan

#include "donner/base/parser/LengthParser.h"

namespace donner::parser {

namespace {

/// Exercise LengthParser::Parse with the given options, asserting basic invariants that must hold
/// regardless of input.
void tryParse(std::string_view buffer, LengthParser::Options options) {
  auto result = LengthParser::Parse(buffer, options);
  if (result.hasResult()) {
    assert(result.result().consumedChars <= buffer.size() &&
           "consumedChars should not exceed input size");
    assert(!std::isnan(result.result().length.value) && "Parsed length should not be NaN");
  }
}

}  // namespace

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }

  // Use the first byte to select the option combination, and feed the rest as the string to
  // parse. This exercises all four combinations of unitOptional/limitUnitToPercentage.
  const uint8_t optionByte = data[0];
  // NOLINTNEXTLINE: Allow reinterpret_cast
  const std::string_view buffer(reinterpret_cast<const char*>(data) + 1, size - 1);

  LengthParser::Options options;
  options.unitOptional = (optionByte & 0x1) != 0;
  options.limitUnitToPercentage = (optionByte & 0x2) != 0;

  tryParse(buffer, options);

  // Also exercise the standalone unit parser.
  auto unitResult = LengthParser::ParseUnit(buffer);
  (void)unitResult;

  return 0;
}

}  // namespace donner::parser
