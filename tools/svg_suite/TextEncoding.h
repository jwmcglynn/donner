#pragma once
/// @file
/// Bounded UTF-8 identities and explicitly lossy diagnostic displays for suite evidence.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace donner::svg_suite {
inline constexpr size_t kMaxIdentityBytes = 65536;
inline constexpr size_t kMaxArgumentBytes = 1024 * 1024;
inline constexpr size_t kMaxArguments = 10000;
inline constexpr size_t kMaxDiagnosticBytes = 65536;

/// Validate a bounded, NUL-free canonical UTF-8 identity without changing its bytes.
bool IsValidIdentityText(std::string_view text, size_t maxBytes = kMaxIdentityBytes);

/// Return an exact bounded UTF-8 path spelling, or nullopt for malformed native text.
/// Empty paths are encoding-valid; required-path semantics belong to the caller.
std::optional<std::string> Utf8Path(const std::filesystem::path& path);

/// Validate argument count, individual encoding and aggregate bytes before launching a child.
std::optional<size_t> ArgumentBytes(std::span<const std::string> arguments);

/// Display at most 64 KiB of diagnostics, replacing each malformed byte with U+FFFD.
/// The input may contain NUL. This lossy display must never encode an identity or filesystem key.
std::string DiagnosticUtf8(std::string_view text);
}  // namespace donner::svg_suite
