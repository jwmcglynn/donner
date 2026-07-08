#include "donner/css/parser/RuleParser.h"

namespace donner::css::parser {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view str(reinterpret_cast<const char*>(data),  // NOLINT: Intentional cast
                             size);

  // Exercise all three top-level entry points, which use distinct parsing algorithms per
  // https://www.w3.org/TR/css-syntax-3/#parser-entry-points
  auto stylesheetResult = RuleParser::ParseStylesheet(str);
  (void)stylesheetResult;

  auto listOfRulesResult = RuleParser::ParseListOfRules(str);
  (void)listOfRulesResult;

  auto ruleResult = RuleParser::ParseRule(str);
  (void)ruleResult;

  return 0;
}

}  // namespace donner::css::parser
