#include "tools/svg_suite/TextEncoding.h"

#include <cstdint>
#include <type_traits>

namespace donner::svg_suite {
namespace {
size_t SequenceLength(unsigned char first) {
  if (first < 0x80) {
    return 1;
  }
  if (first >= 0xC2 && first <= 0xDF) {
    return 2;
  }
  if (first >= 0xE0 && first <= 0xEF) {
    return 3;
  }
  return first >= 0xF0 && first <= 0xF4 ? 4 : 0;
}

bool ValidScalar(uint32_t scalar, size_t count) {
  constexpr uint32_t kMinimum[] = {0, 0, 0x80, 0x800, 0x10000};
  return scalar >= kMinimum[count] && scalar <= 0x10FFFF && !(scalar >= 0xD800 && scalar <= 0xDFFF);
}

size_t ScalarLength(std::string_view text) {
  if (text.empty()) {
    return 0;
  }
  const auto first = static_cast<unsigned char>(text.front());
  const size_t count = SequenceLength(first);
  if (count == 0 || text.size() < count || count == 1) {
    return count == 1 ? 1 : 0;
  }
  uint32_t scalar = first & (0x7F >> count);
  for (size_t index = 1; index < count; ++index) {
    const auto continuation = static_cast<unsigned char>(text[index]);
    if ((continuation & 0xC0) != 0x80) {
      return 0;
    }
    scalar = (scalar << 6) | (continuation & 0x3F);
  }
  return ValidScalar(scalar, count) ? count : 0;
}

[[maybe_unused]] bool ConsumeWideScalar(std::wstring_view text, size_t& index) {
  const auto scalar = static_cast<uint32_t>(text[index]);
  if (scalar == 0 || scalar > 0x10FFFF) {
    return false;
  }
  if constexpr (sizeof(wchar_t) == 2) {
    if (scalar >= 0xD800 && scalar <= 0xDBFF) {
      if (++index == text.size()) {
        return false;
      }
      const auto low = static_cast<uint32_t>(text[index]);
      return low >= 0xDC00 && low <= 0xDFFF;
    }
  }
  return scalar < 0xD800 || scalar > 0xDFFF;
}

// A template keeps the discarded wide branch from requiring a char-to-wstring_view conversion.
template <typename Char>
bool ValidNativePath(const std::basic_string<Char>& native) {
  if (native.size() > kMaxIdentityBytes) {
    return false;
  }
  if constexpr (std::is_same_v<Char, wchar_t>) {
    for (size_t index = 0; index < native.size(); ++index) {
      if (!ConsumeWideScalar(native, index)) {
        return false;
      }
    }
    return true;
  } else {
    return IsValidIdentityText(native);
  }
}
}  // namespace

bool IsValidIdentityText(std::string_view text, size_t maxBytes) {
  if (text.size() > maxBytes) {
    return false;
  }
  while (!text.empty()) {
    const size_t count = ScalarLength(text);
    if (count == 0 || text.front() == '\0') {
      return false;
    }
    text.remove_prefix(count);
  }
  return true;
}

std::optional<std::string> Utf8Path(const std::filesystem::path& path) {
  if (!ValidNativePath(path.native())) {
    return std::nullopt;
  }
  const std::u8string encoded = path.generic_u8string();
  const std::string text(reinterpret_cast<const char*>(encoded.data()), encoded.size());
  return IsValidIdentityText(text) ? std::optional(text) : std::nullopt;
}

std::optional<size_t> ArgumentBytes(std::span<const std::string> arguments) {
  if (arguments.size() > kMaxArguments) {
    return std::nullopt;
  }
  size_t bytes = 0;
  for (const auto& argument : arguments) {
    if (argument.size() > kMaxArgumentBytes - bytes || !IsValidIdentityText(argument)) {
      return std::nullopt;
    }
    bytes += argument.size();
  }
  return bytes;
}

std::string DiagnosticUtf8(std::string_view text) {
  const bool truncated = text.size() > kMaxDiagnosticBytes;
  text = text.substr(0, kMaxDiagnosticBytes);
  std::string result;
  while (!text.empty()) {
    const size_t count = ScalarLength(text);
    if (count == 0) {
      result += "\xEF\xBF\xBD";
      text.remove_prefix(1);
    } else {
      result.append(text.substr(0, count));
      text.remove_prefix(count);
    }
  }
  if (truncated) {
    result += "[diagnostic truncated]";
  }
  return result;
}
}  // namespace donner::svg_suite
