#include "donner/base/ParseDiagnostic.h"

namespace donner {

std::ostream& operator<<(std::ostream& os, const ParseDiagnostic& diag) {
  os << diag.severity << " at ";
  if (diag.range.start.lineInfo) {
    os << diag.range.start.lineInfo.value();
  } else if (diag.range.start.offset) {
    os << "0:" << diag.range.start.offset.value();
  } else {
    os << "<eos>";
  }

  return os << ": " << diag.reason;
}

}  // namespace donner
