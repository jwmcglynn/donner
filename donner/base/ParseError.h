#pragma once
/// @file

#include <ostream>
#include <string>

#include "donner/base/FileOffset.h"

namespace donner::base::parser {

/**
 * Error context for a failed parse, such as the error reason, line, and character offset.
 */
struct ParseError {
  /// Error message string.
  std::string reason;

  /// Location of the error, containing a character offset and optional line number.
  FileOffset location = FileOffset::Offset(0);

  /// Ostream output operator for \ref ParseError, outputs the error message.
  friend std::ostream& operator<<(std::ostream& os, const ParseError& error);
};

}  // namespace donner::base::parser

// Re-export in svg and css namespaces for convenience.
namespace donner::svg::parser {
using donner::base::parser::ParseError;
}  // namespace donner::svg::parser

namespace donner::css::parser {
using donner::base::parser::ParseError;
}  // namespace donner::css::parser
