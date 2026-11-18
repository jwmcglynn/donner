#pragma once
/// @file
/// Lists a test font directory's font files in the order the test harnesses register them.

#include <filesystem>
#include <system_error>
#include <vector>

namespace donner::svg {

/**
 * Lists the `.ttf` and `.otf` entries of a font directory, sorted by path.
 *
 * Coverage fallback tries faces in registration order, so the harnesses register fonts in this
 * order rather than in the filesystem's listing order, as resvg's test harness does. Callers apply
 * their own checks to each entry.
 *
 * @param directory Directory to list.
 * @param error Set when the directory cannot be listed, in which case the result is empty.
 * @return The font file paths, sorted.
 */
std::vector<std::filesystem::path> ListFontFilesSorted(const std::filesystem::path& directory,
                                                       std::error_code& error);

}  // namespace donner::svg
