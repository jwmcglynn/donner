#include "donner/editor/tests/BitmapCompareCommon.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace donner::editor::tests::detail {

std::string Flatten(std::string_view path) {
  std::string out;
  out.reserve(path.size());
  for (char c : path) {
    out.push_back(c == '/' || c == '\\' ? '_' : c);
  }
  return out;
}

std::filesystem::path DiffOutputDir() {
  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR"); dir != nullptr) {
    return std::filesystem::path(dir);
  }
  return std::filesystem::temp_directory_path();
}

std::vector<uint8_t> BuildSideBySide(const std::vector<uint8_t>& expected, int expectedWidth,
                                     int expectedHeight, std::size_t expectedStrideInPixels,
                                     const std::vector<uint8_t>& actual, int actualWidth,
                                     int actualHeight, std::size_t actualStrideInPixels) {
  const int combinedWidth = expectedWidth + actualWidth;
  const int combinedHeight = std::max(expectedHeight, actualHeight);
  const std::size_t combinedStride = static_cast<std::size_t>(combinedWidth);
  std::vector<uint8_t> combined(combinedStride * static_cast<std::size_t>(combinedHeight) * 4u, 0u);

  for (int y = 0; y < expectedHeight; ++y) {
    const std::size_t srcOffset = static_cast<std::size_t>(y) * expectedStrideInPixels * 4u;
    uint8_t* dst = combined.data() + static_cast<std::size_t>(y) * combinedStride * 4u;
    std::memcpy(dst, expected.data() + srcOffset, static_cast<std::size_t>(expectedWidth) * 4u);
  }
  for (int y = 0; y < actualHeight; ++y) {
    const std::size_t srcOffset = static_cast<std::size_t>(y) * actualStrideInPixels * 4u;
    uint8_t* dst = combined.data() + static_cast<std::size_t>(y) * combinedStride * 4u +
                   static_cast<std::size_t>(expectedWidth) * 4u;
    std::memcpy(dst, actual.data() + srcOffset, static_cast<std::size_t>(actualWidth) * 4u);
  }
  return combined;
}

}  // namespace donner::editor::tests::detail
