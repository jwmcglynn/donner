#include "src/css/declaration.h"

namespace donner {
namespace css {

Function::Function(std::string name) : name(std::move(name)) {}

bool Function::operator==(const Function& other) const {
  return name == other.name && values == other.values;
}

SimpleBlock::SimpleBlock(TokenIndex associatedToken) : associatedToken(associatedToken) {}

bool SimpleBlock::operator==(const SimpleBlock& other) const {
  return associatedToken == other.associatedToken && values == other.values;
}

AtRule::AtRule(std::string name) : name(std::move(name)) {}

bool AtRule::operator==(const AtRule& other) const {
  return name == other.name && prelude == other.prelude && block == other.block;
}

ComponentValue::ComponentValue(ComponentValue::Type&& value) : value(std::move(value)) {}

bool ComponentValue::operator==(const ComponentValue& other) const {
  return value == other.value;
}

DeclarationOrAtRule::DeclarationOrAtRule(DeclarationOrAtRule::Type&& value)
    : value(std::move(value)) {}

bool DeclarationOrAtRule::operator==(const DeclarationOrAtRule& other) const {
  return value == other.value;
}

}  // namespace css
}  // namespace donner
