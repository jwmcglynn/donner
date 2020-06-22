#include "src/base/parser/parse_error.h"

namespace donner {

std::ostream& operator<<(std::ostream& os, const ParseError& error) {
  os << "Parse error at " << error.line << ":" << error.offset << ": " << error.reason;
  return os;
}

}  // namespace donner
