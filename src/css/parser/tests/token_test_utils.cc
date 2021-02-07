#include "src/css/parser/tests/token_test_utils.h"

#include <ostream>

#include "src/css/token.h"

namespace donner {
namespace css {

void PrintTo(const Token& token, std::ostream* os) {
  *os << "Token { ";
  token.visit([&os](auto&& value) { *os << value; });
  *os << " offset: " << token.offset();
  *os << " }";
}

}  // namespace css
}  // namespace donner
