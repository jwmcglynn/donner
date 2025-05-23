#include "donner/css/parser/DeclarationListParser.h"

namespace donner::css::parser {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
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
