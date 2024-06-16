#include "src/base/parser/parse_error.h"

namespace donner::base::parser {

std::ostream& operator<<(std::ostream& os, const ParseError& error) {
  os << "Parse error at " << error.line << ":" << error.offset << ": " << error.reason;
  return os;
}

}  // namespace donner::base::parser
