#include "src/css/parser/declaration_list_parser.h"

namespace donner::css::parser {

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
