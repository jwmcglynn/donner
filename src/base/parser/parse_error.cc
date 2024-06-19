#include "src/base/parser/parse_error.h"

namespace donner::base::parser {

std::ostream& operator<<(std::ostream& os, const ParseError& error) {
  os << "Parse error at " << error.location.line << ":";
  if (error.location.offset.has_value()) {
    os << error.location.offset.value();
    os << ": ";
  } else {
    os << "<eol>: ";
  }

  return os << error.reason;
}

}  // namespace donner::base::parser
