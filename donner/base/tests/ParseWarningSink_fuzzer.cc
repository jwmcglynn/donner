#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"

namespace donner {

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  ParseWarningSink sink;

  if (input == "warning-count-budget") {
    for (std::size_t i = 0; i <= ParseWarningSink::kMaximumWarnings; ++i) {
      sink.add(ParseDiagnostic::Warning("x", FileOffset::Offset(i)));
    }
  } else if (input == "warning-byte-budget") {
    sink.add(ParseDiagnostic::Warning(
        RcString(std::string(ParseWarningSink::kMaximumWarningBytes + 1, 'x')),
        FileOffset::Offset(0)));
  } else {
    sink.add(ParseDiagnostic::Warning(RcString(input), FileOffset::Offset(0)));
  }

  if (sink.warnings().size() > ParseWarningSink::kMaximumWarnings ||
      sink.warningBytes() > ParseWarningSink::kMaximumWarningBytes) {
    std::abort();
  }
  if ((input == "warning-count-budget" || input == "warning-byte-budget") &&
      !sink.resourceLimitExceeded()) {
    std::abort();
  }
  return 0;
}

}  // namespace donner
