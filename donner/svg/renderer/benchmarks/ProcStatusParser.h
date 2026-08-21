#pragma once
/// @file
/// Strict parser for numeric KiB fields in Linux `/proc/self/status` lines.

#include <cstdint>
#include <optional>
#include <string_view>

namespace donner::benchmarks {

/// Parse one named `/proc/self/status` field with a `kB` suffix.
///
/// @param line Complete status line, for example `VmRSS: 1234 kB`.
/// @param field Field prefix including the colon, for example `VmRSS:`.
/// @return Parsed KiB value, or nullopt for mismatched, malformed, or overflowing input.
[[nodiscard]] std::optional<std::uint64_t> ParseProcStatusKilobytes(std::string_view line,
                                                                    std::string_view field);

}  // namespace donner::benchmarks
