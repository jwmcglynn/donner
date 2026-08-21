#include "donner/svg/renderer/benchmarks/ProcStatusParser.h"

#include <charconv>
#include <system_error>

namespace donner::benchmarks {
namespace {

bool IsAsciiWhitespace(char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

void TrimAsciiWhitespace(std::string_view& value) {
  while (!value.empty() && IsAsciiWhitespace(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && IsAsciiWhitespace(value.back())) {
    value.remove_suffix(1);
  }
}

}  // namespace

std::optional<std::uint64_t> ParseProcStatusKilobytes(std::string_view line,
                                                      std::string_view field) {
  if (field.empty() || !line.starts_with(field)) {
    return std::nullopt;
  }

  line.remove_prefix(field.size());
  TrimAsciiWhitespace(line);

  std::uint64_t value = 0;
  const std::from_chars_result result =
      std::from_chars(line.data(), line.data() + line.size(), value);
  if (result.ec != std::errc() || result.ptr == line.data()) {
    return std::nullopt;
  }

  std::string_view suffix(result.ptr, line.data() + line.size());
  TrimAsciiWhitespace(suffix);
  if (suffix != "kB") {
    return std::nullopt;
  }

  return value;
}

}  // namespace donner::benchmarks
