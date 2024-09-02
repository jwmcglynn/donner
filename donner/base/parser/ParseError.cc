#include "donner/base/parser/ParseError.h"

namespace donner::base::parser {

/// Ostream output operator for \ref ParseError, outputs the error message.
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
