#include <cstdlib>
#include <string>

#include "donner/base/ParseWarningSink.h"
#include "donner/css/parser/DeclarationListParser.h"
#include "donner/css/parser/RuleParser.h"
#include "donner/css/parser/StylesheetParser.h"

namespace donner::css::parser {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);  // NOLINT
  std::string generated;
  if (input == "stylesheet-rule-budget") {
    for (std::size_t i = 0; i <= RuleParser::kMaximumRules; ++i) {
      generated += "r" + std::to_string(i) + "{fill:red}";
    }
  } else if (input == "stylesheet-rule-declaration-budget") {
    generated = "rect{";
    for (std::size_t i = 0; i <= DeclarationListParser::kMaximumDeclarations; ++i) {
      generated += "fill:red;";
    }
    generated += "}";
  } else if (input == "stylesheet-aggregate-declaration-budget") {
    for (std::size_t rule = 0; rule < 5; ++rule) {
      generated += "r" + std::to_string(rule) + "{";
      for (std::size_t i = 0; i < DeclarationListParser::kMaximumDeclarations; ++i) {
        generated += "fill:red;";
      }
      generated += "}";
    }
  } else if (input.starts_with("stylesheet-component-budget")) {
    constexpr std::size_t kValuesPerRule = 64;
    constexpr std::size_t kRules = RuleParser::kMaximumComponentValues / kValuesPerRule + 1;
    for (std::size_t rule = 0; rule < kRules; ++rule) {
      generated += "r" + std::to_string(rule) + "{x:";
      for (std::size_t value = 0; value < kValuesPerRule; ++value) {
        generated += "a ";
      }
      generated += "}";
    }
  }

  ParseWarningSink disabled = ParseWarningSink::Disabled();
  const bool boundaryMarker = !generated.empty();
  const Stylesheet result =
      StylesheetParser::Parse(boundaryMarker ? std::string_view(generated) : input, disabled);
  if (result.rules().size() > RuleParser::kMaximumRules ||
      result.securityStats().declarations > Stylesheet::kMaximumDeclarations ||
      result.securityStats().componentValues > RuleParser::kMaximumComponentValues ||
      (boundaryMarker && !result.securityStats().rejected)) {
    std::abort();
  }

  return 0;
}

}  // namespace donner::css::parser
