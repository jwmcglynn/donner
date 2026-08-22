#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "donner/css/parser/DeclarationListParser.h"

namespace donner::css::parser {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  constexpr std::string_view kAggregateValuesSeed = "aggregate-component-values\n";
  if (size == kAggregateValuesSeed.size() &&
      std::memcmp(data, kAggregateValuesSeed.data(), size) == 0) {
    const auto makeDeclaration = [](std::string prefix) {
      for (std::size_t i = 0; i <= DeclarationListParser::kMaximumComponentValues; ++i) {
        prefix += "0 ";
      }
      return prefix;
    };
    const auto makeAtRules = [] {
      std::string result;
      for (std::size_t rule = 0; rule < 5; ++rule) {
        result += "@unknown ";
        for (std::size_t i = 0; i < 8000; ++i) {
          result += "0 ";
        }
        result += ";";
      }
      return result;
    };

    DeclarationListParser::SecurityStats declarationStats;
    const auto declarations = DeclarationListParser::ParseOnlyDeclarations(
        makeDeclaration("unknown:"), &declarationStats);
    DeclarationListParser::SecurityStats atRuleStats;
    const auto atRules = DeclarationListParser::Parse(makeAtRules(), &atRuleStats);
    if (!declarations.empty() || atRules.empty() || !declarationStats.rejected ||
        !atRuleStats.rejected ||
        declarationStats.componentValues != DeclarationListParser::kMaximumComponentValues ||
        atRuleStats.componentValues != DeclarationListParser::kMaximumComponentValues) {
      std::abort();
    }
    return 0;
  }

  auto result = DeclarationListParser::Parse(
      std::string_view(reinterpret_cast<const char*>(data),  // NOLINT: intentional cast
                       size));
  (void)result;

  auto resultOnlyDecls = DeclarationListParser::ParseOnlyDeclarations(
      std::string_view(reinterpret_cast<const char*>(data),  // NOLINT: intentional cast
                       size));
  (void)resultOnlyDecls;

  return 0;
}

}  // namespace donner::css::parser
