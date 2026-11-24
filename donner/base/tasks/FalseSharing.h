#pragma once
/// @file
/// The alignment that keeps data written by different threads off one cache line.

#include <cstddef>

namespace donner::tasks {

/**
 * Bytes that separate data different threads write, so a write by one does not invalidate a line
 * another is reading. 128 rather than one 64-byte line: some cores fetch lines in adjacent pairs,
 * and Apple silicon's lines are 128 bytes. Used as an `alignas` value.
 */
inline constexpr std::size_t kFalseSharingBytes = 128;

}  // namespace donner::tasks
