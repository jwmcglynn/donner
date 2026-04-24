#pragma once
/// @file
///
/// Small byte-prefix heuristic used by the address-bar load flow to turn
/// "Failed to parse SVG" into something actionable when the bytes the
/// user fetched are obviously not SVG (e.g. the common case of pasting a
/// Wikipedia file-description page URL and getting back HTML).
///
/// Not a general-purpose MIME sniffer — the parser is authoritative for
/// anything XML-shaped. This just catches the easy "that wasn't the
/// file, that was a web page" class of mistake before the parser's
/// diagnostic (e.g. `unexpected token '<!DOCTYPE'`) reaches the user.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace donner::editor {

/// If \p bytes obviously aren't SVG (HTML page, JSON, empty, non-XML
/// binary), returns a short human-readable description suitable for the
/// address-bar error chip. Otherwise returns `std::nullopt` and the
/// caller should defer to the parser's own diagnostics.
[[nodiscard]] std::optional<std::string> DescribeNonSvgBytes(std::span<const uint8_t> bytes);

}  // namespace donner::editor
