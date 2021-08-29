#include "src/svg/properties/property_registry.h"

#include "src/css/parser/color_parser.h"
#include "src/css/parser/declaration_list_parser.h"

namespace donner {

namespace {

template <typename T>
std::optional<ParseError> unwrap(ParseResult<T>&& result, std::optional<T>* destination) {
  if (result.hasError()) {
    return std::move(result.error());
  }

  *destination = std::move(result.result());
  return std::nullopt;
}

static constexpr frozen::unordered_map<frozen::string, ParseFunction, 1> kProperties = {
    {"color",
     [](PropertyRegistry& registry, const css::Declaration& declaration) {
       return unwrap(css::ColorParser::Parse(declaration.values), &registry.color);
     }}  //
};

}  // namespace

std::optional<ParseError> PropertyRegistry::parseProperty(const css::Declaration& declaration) {
  const auto it = kProperties.find(frozen::string(declaration.name));
  if (it != kProperties.end()) {
    return it->second(*this, declaration);
  }

  ParseError err;
  err.reason = "Unknown property '" + declaration.name + "'";
  err.offset = declaration.sourceOffset;
  return err;
}

void PropertyRegistry::parseStyle(std::string_view str) {
  const std::vector<css::Declaration> declarations =
      css::DeclarationListParser::ParseOnlyDeclarations(str);
  for (const auto& declaration : declarations) {
    [[maybe_unused]] auto error = parseProperty(declaration);
    // Ignore errors.
  }
}

}  // namespace donner
