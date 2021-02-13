#include "src/css/parser/declaration_list_parser.h"

namespace donner {
namespace css {

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto result =
      DeclarationListParser::Parse(std::string_view(reinterpret_cast<const char*>(data), size));
  (void)result;

  auto resultOnlyDecls = DeclarationListParser::ParseOnlyDeclarations(
      std::string_view(reinterpret_cast<const char*>(data), size));
  (void)resultOnlyDecls;

  return 0;
}

}  // namespace css
}  // namespace donner
