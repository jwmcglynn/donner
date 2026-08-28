#pragma once
/// @file
/// Reading and deliberately rewriting the committed byte-exact shader goldens.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "donner/base/tests/Runfiles.h"

namespace donner::gpu::shader {

/// Directory the goldens live in, relative to both the runfiles root and the repository root.
inline constexpr const char* kShaderGoldenDir = "donner/gpu/shader/tests/testdata/";

/// Packs SPIR-V words little-endian, matching the on-disk golden encoding.
/// @param words Module words in emission order.
inline std::string SpirvWordsToBytes(const std::vector<uint32_t>& words) {
  std::string bytes;
  bytes.reserve(words.size() * 4);
  for (const uint32_t word : words) {
    bytes.push_back(static_cast<char>(word & 0xFF));
    bytes.push_back(static_cast<char>((word >> 8) & 0xFF));
    bytes.push_back(static_cast<char>((word >> 16) & 0xFF));
    bytes.push_back(static_cast<char>((word >> 24) & 0xFF));
  }
  return bytes;
}

/// Reads a committed golden. @param name Golden file name, e.g. `color_matrix.wgsl`.
inline std::string ReadShaderGolden(const std::string& name) {
  const std::string path =
      donner::Runfiles::instance().Rlocation(std::string(kShaderGoldenDir) + name);
  std::ifstream stream(path, std::ios::binary);
  EXPECT_TRUE(stream.good()) << "Failed to open golden file: " << path;
  std::ostringstream contents;
  contents << stream.rdbuf();
  return contents.str();
}

/**
 * Rewrites a golden when its update environment variable names a repository root, and returns
 * true so the caller can skip the comparison. The variable must point at a source checkout: a
 * sandboxed or remotely executed test writes somewhere that is discarded.
 *
 * @param environmentVariable Name of the update variable, e.g. `UPDATE_WGSL_GOLDEN`.
 * @param name Golden file name, e.g. `color_matrix.wgsl`.
 * @param contents Bytes to write.
 */
inline bool MaybeUpdateShaderGolden(const char* environmentVariable, const std::string& name,
                                    const std::string& contents) {
  const char* updateRoot = std::getenv(environmentVariable);
  if (updateRoot == nullptr) {
    return false;
  }
  const std::string outPath = std::string(updateRoot) + "/" + kShaderGoldenDir + name;
  std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
  EXPECT_TRUE(out.good()) << "Failed to open " << outPath << " for writing";
  out << contents;
  return true;
}

}  // namespace donner::gpu::shader
