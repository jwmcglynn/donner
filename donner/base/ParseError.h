#pragma once
/// @file

#include <ostream>

#include "donner/base/CompilerConfig.h"
#include "donner/base/FileOffset.h"
#include "donner/base/RcString.h"

namespace donner {

/**
 * Error context for a failed parse, such as the error reason, line, and character offset.
 */
struct ParseError {
  /// Error message string.
  RcString reason;

  /// Location of the error, containing a character offset and optional line number.
  FileOffset location = FileOffset::Offset(0);

  /// Ostream output operator for \ref ParseError, outputs the error message.
  friend std::ostream& operator<<(std::ostream& os, const ParseError& error);
};

}  // namespace donner
