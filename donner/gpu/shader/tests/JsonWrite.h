#pragma once
/// @file
/// Minimal JSON string output for test-only frozen shader manifests.

#include <cstdio>
#include <string_view>

namespace donner::gpu::shader::tests {

/// Writes one string to stdout with JSON escaping.
/// @param value Shader text or a fixed artifact name.
inline void WriteJsonString(std::string_view value) {
  std::putchar('"');
  for (unsigned char byte : value) {
    switch (byte) {
      case '"': std::fputs("\\\"", stdout); break;
      case '\\': std::fputs("\\\\", stdout); break;
      case '\n': std::fputs("\\n", stdout); break;
      case '\r': std::fputs("\\r", stdout); break;
      case '\t': std::fputs("\\t", stdout); break;
      default:
        if (byte < 0x20) {
          std::fprintf(stdout, "\\u%04x", byte);
        } else {
          std::putchar(byte);
        }
    }
  }
  std::putchar('"');
}

}  // namespace donner::gpu::shader::tests
