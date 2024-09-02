#include "donner/css/parser/SelectorParser.h"
#include "donner/css/parser/details/ComponentValueParser.h"

namespace donner::css::parser {

namespace {

std::vector<ComponentValue> TokenizeString(std::string_view str) {
  details::Tokenizer tokenizer_(str);
  return details::parseListOfComponentValues(tokenizer_);
}

}  // namespace

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto result = SelectorParser::Parse(
      std::string_view(reinterpret_cast<const char*>(data),  // NOLINT: Intentional cast
                       size));
  (void)result;

  // Convert to ComponentValues to validate the other two parse APIs.
  auto components = TokenizeString(
      std::string_view(reinterpret_cast<const char*>(data),  // NOLINT: Intentional cast
                       size));

  auto resultComponents = SelectorParser::ParseComponents(components);
  (void)resultComponents;

  auto resultComponentsForgiving = SelectorParser::ParseForgivingSelectorList(components);
  (void)resultComponentsForgiving;

  auto resultComponentsRelativeForgiving =
      SelectorParser::ParseForgivingRelativeSelectorList(components);
  (void)resultComponentsRelativeForgiving;

  return 0;
}

}  // namespace donner::css::parser
