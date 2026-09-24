#pragma once
/// @file
/// Shared diagnostics for the repository's pixelmatch test helpers.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace donner::editor::tests::detail {

std::string Flatten(std::string_view path);
std::filesystem::path DiffOutputDir();
std::vector<uint8_t> BuildSideBySide(const std::vector<uint8_t>& expected, int expectedWidth,
                                     int expectedHeight, std::size_t expectedStrideInPixels,
                                     const std::vector<uint8_t>& actual, int actualWidth,
                                     int actualHeight, std::size_t actualStrideInPixels);

}  // namespace donner::editor::tests::detail
