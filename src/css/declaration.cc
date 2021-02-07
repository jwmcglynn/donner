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

}  // namespace css
}  // namespace donner
