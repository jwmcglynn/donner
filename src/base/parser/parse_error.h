#pragma once

#include <ostream>
#include <string>

namespace donner {

struct ParseError {
  std::string reason;
  int line = 0;
  int offset = 0;

  // Output.
  friend std::ostream& operator<<(std::ostream& os, const ParseError& error);
};

}  // namespace donner
