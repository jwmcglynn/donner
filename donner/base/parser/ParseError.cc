#include "donner/base/parser/ParseError.h"

namespace donner::base::parser {

/// Ostream output operator for \ref ParseError, outputs the error message.
std::ostream& operator<<(std::ostream& os, const ParseError& error) {
  os << "Parse error at ";
  if (error.location.lineInfo) {
    os << error.location.lineInfo.value();
  } else if (error.location.offset) {
    os << "0:" << error.location.offset.value();
  } else {
    os << "<eos>";
  }

  return os << ": " << error.reason;
}

}  // namespace donner::base::parser
